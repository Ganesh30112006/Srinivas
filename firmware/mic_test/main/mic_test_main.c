/*
 * Hardware validation: INMP441 → ESP32 I2S → 16 kHz 16-bit mono WAV over the USB console.
 *
 * Wiring: pins in firmware/board/board.h. ESP32 DevKit V1: SCK=D26, WS=D25, SD=D33 (100 kΩ to GND);
 * ESP32-S3: SCK=GPIO4, WS=GPIO5, SD=GPIO6. L/R=GND, VDD=3V3 with 100 nF at the mic. The main
 * (or Start) button records again. Boards without PSRAM record 5 s instead of 10 s.
 *
 * Output protocol (text, so it survives the console):
 *   LEVEL t=<s> rms=<dBFS> peak=<dBFS>          every 500 ms while recording
 *   DIAG <ok|silent|clipping|dc_offset|stuck> <detail>
 *   WAV BEGIN <bytes> <crc32>
 *   <base64 lines, 76 chars>
 *   WAV END
 * Capture with: python firmware/tools/capture_wav.py --port COMx --out mic.wav
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_flashlog.h" /* lc_crc32 */
#include "mbedtls/base64.h"

#define SAMPLE_RATE 16000
#if CONFIG_SPIRAM
#define RECORD_SECONDS 10
#else
#define RECORD_SECONDS 5 /* 160 KB in internal RAM */
#endif
#define TOTAL_SAMPLES (SAMPLE_RATE * RECORD_SECONDS)
#define READ_FRAMES 512
#define PIN_BCLK PIN_I2S_BCLK
#define PIN_WS PIN_I2S_WS
#define PIN_SD PIN_I2S_DIN
#if BOARD_ONE_BUTTON
#define PIN_START PIN_BTN_MAIN
#else
#define PIN_START PIN_BTN_START
#endif
#define GAIN_Q8 (16 * 256) /* 24-bit → 16-bit with +24 dB: mic test needs audible speech */

static const char *TAG = "mic_test";
static i2s_chan_handle_t s_rx;

static void i2s_setup(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = READ_FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
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
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* L/R tied to GND */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static double dbfs(double value) {
    return value <= 0 ? -120.0 : 20.0 * log10(value / 32768.0);
}

static void record(int16_t *pcm) {
    static int32_t raw[READ_FRAMES];
    size_t written = 0;
    int32_t dc = 0;
    int32_t prev_x = 0;
    int32_t prev_y = 0;
    double sum_sq = 0;
    int32_t peak = 0;
    size_t clipped = 0;
    int64_t dc_sum = 0;
    bool all_same = true;
    int32_t first = 0;
    bool first_set = false;

    /* Discard 200 ms: the INMP441 needs time after the clock starts. */
    for (int i = 0; i < 7; i++) {
        size_t got = 0;
        i2s_channel_read(s_rx, raw, sizeof raw, &got, portMAX_DELAY);
    }
    while (written < TOTAL_SAMPLES) {
        size_t got = 0;
        ESP_ERROR_CHECK(i2s_channel_read(s_rx, raw, sizeof raw, &got, portMAX_DELAY));
        const size_t frames = got / sizeof(int32_t);
        for (size_t i = 0; i < frames && written < TOTAL_SAMPLES; i++) {
            const int32_t x = raw[i] >> 8; /* 24-bit sample, MSB-aligned in the 32-bit slot */
            if (!first_set) {
                first = raw[i];
                first_set = true;
            } else if (raw[i] != first) {
                all_same = false;
            }
            dc_sum += x;
            /* DC-blocking high-pass: y[n] = x[n] - x[n-1] + 0.995 y[n-1] (Q15). */
            const int32_t y = x - prev_x + (int32_t)(((int64_t)prev_y * 32604) >> 15);
            prev_x = x;
            prev_y = y;
            int32_t s = (int32_t)(((int64_t)y * GAIN_Q8) >> 16);
            if (s > 32767 || s < -32768) {
                clipped++;
                s = s > 0 ? 32767 : -32768;
            }
            pcm[written++] = (int16_t)s;
            sum_sq += (double)s * s;
            const int32_t mag = s < 0 ? -s : s;
            if (mag > peak) peak = mag;
            if (written % (SAMPLE_RATE / 2) == 0) {
                printf("LEVEL t=%.1f rms=%.1f peak=%.1f\n", (double)written / SAMPLE_RATE,
                       dbfs(sqrt(sum_sq / (SAMPLE_RATE / 2))), dbfs(peak));
                sum_sq = 0;
                peak = 0;
            }
        }
    }
    dc = (int32_t)(dc_sum / TOTAL_SAMPLES);
    double total_sq = 0;
    for (size_t i = 0; i < TOTAL_SAMPLES; i++) total_sq += (double)pcm[i] * pcm[i];
    const double rms = dbfs(sqrt(total_sq / TOTAL_SAMPLES));
    if (all_same) {
        printf("DIAG stuck raw=0x%08" PRIx32 " (SD floating/shorted, or SCK/WS swapped)\n", (uint32_t)first);
    } else if (rms < -70.0) {
        printf("DIAG silent rms=%.1f dBFS (check VDD=3V3, L/R=GND, SD wiring)\n", rms);
    } else if (clipped > TOTAL_SAMPLES / 1000) {
        printf("DIAG clipping %u samples (move the mic away or lower gain)\n", (unsigned)clipped);
    } else if (dc > (1 << 18) || dc < -(1 << 18)) {
        printf("DIAG dc_offset %" PRId32 " (24-bit units; high-pass removes it, check grounding)\n", dc);
    } else {
        printf("DIAG ok rms=%.1f dBFS dc=%" PRId32 "\n", rms, dc);
    }
}

static void put_le(uint8_t *p, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* `wav` holds a 44-byte header slot followed by the samples (see app_main): no second copy. */
static void emit_wav(uint8_t *wav) {
    const uint32_t data_bytes = TOTAL_SAMPLES * 2;
    const size_t total = 44 + data_bytes;
    memcpy(wav, "RIFF", 4);
    put_le(wav + 4, 36 + data_bytes, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    put_le(wav + 16, 16, 4);
    put_le(wav + 20, 1, 2); /* PCM */
    put_le(wav + 22, 1, 2); /* mono */
    put_le(wav + 24, SAMPLE_RATE, 4);
    put_le(wav + 28, SAMPLE_RATE * 2, 4);
    put_le(wav + 32, 2, 2);
    put_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4);
    put_le(wav + 40, data_bytes, 4);

    printf("WAV BEGIN %u %08" PRIx32 "\n", (unsigned)total, lc_crc32(wav, total));
    unsigned char line[110];
    for (size_t off = 0; off < total; off += 57) {
        const size_t n = (total - off) < 57 ? (total - off) : 57;
        size_t olen = 0;
        mbedtls_base64_encode(line, sizeof line, &olen, wav + off, n);
        line[olen] = '\0';
        printf("%s\n", line);
        if ((off / 57) % 64 == 0) vTaskDelay(1); /* let the console drain */
    }
    printf("WAV END\n");
}

void app_main(void) {
    uint8_t *wav = heap_caps_malloc(44 + TOTAL_SAMPLES * sizeof(int16_t), BOARD_BIG_CAPS);
    ESP_ERROR_CHECK(wav == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    int16_t *pcm = (int16_t *)(wav + 44); /* 44 is a multiple of 4: aligned */
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_START,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    i2s_setup();
    ESP_LOGI(TAG, "%s, recording %d s per take", BOARD_NAME, RECORD_SECONDS);
    for (;;) {
        printf("READY recording %d s in 3 s — speak at ~1 m\n", RECORD_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(3000));
        record(pcm);
        emit_wav(wav);
        printf("PRESS THE %s BUTTON (GPIO%d) to record again\n", BOARD_ONE_BUTTON ? "MAIN" : "START", PIN_START);
        while (gpio_get_level(PIN_START) != 0) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
