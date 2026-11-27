#include "audio.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "chunk_store.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_adpcm.h"
#include "lc_frame.h"
#include "session.h"

#define PIN_BCLK PIN_I2S_BCLK
#define PIN_WS PIN_I2S_WS
#define PIN_SD PIN_I2S_DIN
#define READ_FRAMES 512
#if CONFIG_SPIRAM
#define DMA_DESC 8 /* 8 × 512 frames = 256 ms of DMA buffering (covers flash erase stalls) */
#else
#define DMA_DESC 6 /* 192 ms: offline audio goes to the SD card, which never stalls the CPU */
#endif
#define SILENCE_DBFS (-60.0f)
#define SILENCE_CHUNKS 120 /* 60 s */

static const char *TAG = "audio";

#if !CONFIG_LC_AUDIO_SOURCE_TEST_CLIP
static i2s_chan_handle_t open_i2s(void) {
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC;
    chan_cfg.dma_frame_num = READ_FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(LC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = I2S_GPIO_UNUSED,
                .din = PIN_SD,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* INMP441 L/R = GND */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    return rx;
}
#endif

/* One 500 ms chunk is complete: update the level meter and hand it to the chunk store. */
static void chunk_done(const int16_t *chunk, double sum_sq, unsigned *silent_chunks) {
    const float rms = sqrtf((float)(sum_sq / LC_SAMPLES_PER_CHUNK));
    const float db = rms <= 1.0f ? -90.0f : 20.0f * log10f(rms / 32768.0f);
    g_facts.level_db10 = (uint16_t)(db <= -90.0f ? 0 : (db + 90.0f) * 10.0f);
    uint32_t seq;
    uint8_t sid[16];
    if (session_take_seq(&seq, sid)) {
        /* The last sample arrived now, so the chunk started 500 ms ago. */
        const uint64_t now = app_unix_ms();
        chunk_store_push(sid, seq, now == 0 ? 0 : now - 500, chunk, LC_SAMPLES_PER_CHUNK);
        *silent_chunks = db < SILENCE_DBFS ? *silent_chunks + 1 : 0;
        g_facts.mic_silent = g_facts.vad_enabled && *silent_chunks >= SILENCE_CHUNKS;
    } else {
        *silent_chunks = 0;
        g_facts.mic_silent = false;
    }
}

#if CONFIG_LC_AUDIO_SOURCE_TEST_CLIP
/* Test source: plays main/test_clip.adpcm (back-to-back 4004-byte ADPCM chunks) in real time. */
extern const uint8_t test_clip_start[] asm("_binary_test_clip_adpcm_start");
extern const uint8_t test_clip_end[] asm("_binary_test_clip_adpcm_end");

static void audio_task(void *arg) {
    (void)arg;
    static int16_t chunk[LC_SAMPLES_PER_CHUNK];
    const size_t n_chunks = (size_t)(test_clip_end - test_clip_start) / LC_ADPCM_CHUNK_BYTES;
    ESP_LOGW(TAG, "AUDIO SOURCE IS THE TEST CLIP (%u s, looped), not the microphone", (unsigned)(n_chunks / 2));
    unsigned silent_chunks = 0;
    size_t i = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(500));
        const size_t got = lc_adpcm_decode(test_clip_start + i * LC_ADPCM_CHUNK_BYTES, LC_ADPCM_CHUNK_BYTES, chunk,
                                           LC_SAMPLES_PER_CHUNK);
        double sum_sq = 0;
        for (size_t k = 0; k < got; k++) sum_sq += (double)chunk[k] * chunk[k];
        if (got == LC_SAMPLES_PER_CHUNK) chunk_done(chunk, sum_sq, &silent_chunks);
        i = n_chunks == 0 ? 0 : (i + 1) % n_chunks;
    }
}
#else
static void audio_task(void *arg) {
    (void)arg;
    i2s_chan_handle_t rx = open_i2s();
    static int32_t raw[READ_FRAMES];             /* internal RAM */
    static int16_t chunk[LC_SAMPLES_PER_CHUNK];  /* internal RAM: 16 KB */
    size_t filled = 0;
    int32_t prev_x = 0;
    int32_t prev_y = 0;
    double sum_sq = 0;
    unsigned silent_chunks = 0;

    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, raw, sizeof raw, &got, 1000 /* ms */) != ESP_OK) {
            ESP_LOGE(TAG, "I2S read timeout (check SCK/WS wiring)");
            continue;
        }
        const int32_t gain_q8 = (int32_t)(g_facts.gain * 256.0f);
        const size_t frames = got / sizeof(int32_t);
        for (size_t i = 0; i < frames; i++) {
            const int32_t x = raw[i] >> 8; /* 24-bit sample, MSB-aligned in the 32-bit slot */
            /* DC-blocking high-pass, pole 0.995 (Q15): removes the INMP441 offset and rumble. */
            const int32_t y = x - prev_x + (int32_t)(((int64_t)prev_y * 32604) >> 15);
            prev_x = x;
            prev_y = y;
            int32_t s = (int32_t)(((int64_t)y * gain_q8) >> 16); /* 24-bit → 16-bit with gain */
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            chunk[filled++] = (int16_t)s;
            sum_sq += (double)s * s;
            if (filled < LC_SAMPLES_PER_CHUNK) continue;

            chunk_done(chunk, sum_sq, &silent_chunks);
            filled = 0;
            sum_sq = 0;
        }
    }
}

#endif

void audio_start(void) {
    /* Highest application priority, alone on core 1 (Wi-Fi/TLS run on core 0). */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, configMAX_PRIORITIES - 3, NULL, 1);
}
