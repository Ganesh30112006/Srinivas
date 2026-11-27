#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "chunk_store.h"
#include "config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "session.h"
#include "ssd1306.h"
#include "wifi_setup.h"

#define PIN_SDA PIN_I2C_SDA
#define PIN_SCL PIN_I2C_SCL
#define OLED_ADDR 0x3C
#define TICK_MS 250 /* LED blink resolution; OLED refreshes every 2nd tick (2 Hz) */

static const char *TAG = "ui";
static bool s_oled_ok;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t OFF = {0, 0, 0};
static const rgb_t RED = {255, 0, 0};
static const rgb_t GREEN = {0, 255, 0};
static const rgb_t BLUE = {0, 0, 255};
static const rgb_t CYAN = {0, 255, 255};
static const rgb_t YELLOW = {255, 150, 0}; /* red + green mix, tuned for the 220/100 Ω resistors */

static void led_init(void) {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int pins[3] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
    for (int i = 0; i < 3; i++) {
        const ledc_channel_config_t ch = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
}

static void led_set(rgb_t c) {
    const uint8_t duty[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; i++) {
#if CONFIG_LC_LED_COMMON_ANODE
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 255 - duty[i]); /* common anode: low = on */
#else
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty[i]); /* cathode to GND: high = on */
#endif
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
}

/* LED mirrors the web app's status colours (red rec, blue sync, green ready, yellow no network). */
static rgb_t led_for(device_state_t st, unsigned tick) {
    if (wifi_setup_active()) return (tick / 2) % 2 == 0 ? GREEN : OFF; /* slow green blink */
    switch (st) {
        case ST_READY: return GREEN;
        case ST_RECORDING: return RED;
        case ST_RECORDING_OFFLINE: return (tick / 2) % 2 == 0 ? RED : YELLOW; /* 1 Hz alternation */
        case ST_PAUSED: return (tick / 4) % 2 == 0 ? RED : OFF; /* slow red blink: session open, not capturing */
        case ST_SYNCING:
        case ST_FINALIZING: return tick % 2 == 0 ? CYAN : BLUE; /* green flickers: visible with two LEDs */
        case ST_ERROR:
        case ST_WIFI_CONNECTING:
        case ST_SERVER_CONNECTING: return YELLOW;
        case ST_BOOT:
        default: return OFF;
    }
}

static void wifi_bars(char *out, size_t len) {
    const int rssi = g_facts.wifi_rssi;
    const int bars = !g_facts.wifi_up ? 0 : rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    snprintf(out, len, "WiFi %.*s%.*s", bars, "||||", 4 - bars, "....");
}

static void draw(device_state_t st) {
    char line[32];
    char title[64];
    char error[40];
    const session_info_t s = session_info();
    ssd1306_clear();
    if (wifi_setup_active()) {
        ssd1306_text(0, 0, "WI-FI SETUP", 1);
        ssd1306_text(0, 2, "On your phone join:", 1);
        ssd1306_text(0, 3, wifi_setup_ap_name(), 1);
        snprintf(line, sizeof line, "Password %s", wifi_setup_ap_pass());
        ssd1306_text(0, 4, line, 1);
        ssd1306_text(0, 6, "Then open", 1);
        ssd1306_text(0, 7, "192.168.4.1", 1);
        if (s_oled_ok && ssd1306_flush() != ESP_OK) ESP_LOGW(TAG, "OLED write failed");
        return;
    }
    switch (st) {
        case ST_RECORDING:
        case ST_RECORDING_OFFLINE: {
            const uint32_t secs = s.next_seq / 2; /* exact audio duration: 2 chunks per second */
            snprintf(line, sizeof line, "REC %02lu:%02lu:%02lu", (unsigned long)(secs / 3600),
                     (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
            ssd1306_text(0, 0, line, 1);
            if (st == ST_RECORDING_OFFLINE) {
                ssd1306_text(0, 2, "REC (offline)", 1);
                snprintf(line, sizeof line, "RAM %u%% %s %u%%", g_facts.psram_pct, chunk_store_medium(),
                         g_facts.flash_pct);
                ssd1306_text(0, 3, line, 1);
            } else {
                wifi_bars(line, sizeof line);
                ssd1306_text(0, 2, line, 1);
                ssd1306_text(72, 2, "srv OK", 1);
            }
            const int width = (int)g_facts.level_db10 * 124 / 900; /* -90..0 dBFS meter */
            ssd1306_fill_rect(0, 36, 128, 8, false);
            ssd1306_fill_rect(2, 38, width < 0 ? 0 : width, 4, true);
            break;
        }
        case ST_PAUSED: {
            const uint32_t secs = s.next_seq / 2;
            snprintf(line, sizeof line, "PAUSED %02lu:%02lu:%02lu", (unsigned long)(secs / 3600),
                     (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
            ssd1306_text(0, 0, line, 1);
            ssd1306_text(0, 2, "Not recording", 1);
            ssd1306_text(0, 3, BOARD_RESUME_HINT, 1);
            break;
        }
        case ST_SYNCING:
        case ST_FINALIZING:
            ssd1306_text(0, 0, st == ST_SYNCING ? "SYNCING" : "FINISHING", 1);
            snprintf(line, sizeof line, "Syncing %lu chunks", (unsigned long)g_facts.pending_all);
            ssd1306_text(0, 2, line, 1);
            ssd1306_text(0, 3, "Keep device on", 1);
            break;
        case ST_ERROR:
            app_get_error(error, sizeof error);
            ssd1306_text(0, 0, "ERROR", 2);
            ssd1306_text(0, 3, error[0] != '\0' ? error : "Retrying...", 1);
            break;
        case ST_WIFI_CONNECTING:
            ssd1306_text(0, 0, "No Wi-Fi", 1);
            ssd1306_text(0, 2, net_wifi_ssid(), 1);
            ssd1306_text(0, 3, net_wifi_problem(), 1);
            ssd1306_text(0, 4, "Start still records", 1);
            ssd1306_text(0, 6, "Change Wi-Fi: hold", 1);
            ssd1306_text(0, 7, "button 5 s", 1);
            break;
        case ST_SERVER_CONNECTING:
            wifi_bars(line, sizeof line);
            ssd1306_text(0, 0, line, 1);
            ssd1306_text(0, 2, "Server unreachable", 1);
            ssd1306_text(0, 3, "Start still records", 1);
            break;
        case ST_READY:
        default:
            ssd1306_text(0, 0, "READY", 1);
            wifi_bars(line, sizeof line);
            ssd1306_text(48, 0, line, 1);
            ssd1306_text(0, 1, "server OK", 1);
            if (app_get_assigned(NULL, 0, title, sizeof title)) {
                ssd1306_text(0, 3, "Next lecture:", 1);
                char part[22];
                strlcpy(part, title, sizeof part);
                ssd1306_text(0, 4, part, 1);
                if (strlen(title) > 21) {
                    strlcpy(part, title + 21, sizeof part);
                    ssd1306_text(0, 5, part, 1);
                }
            } else {
                ssd1306_text(0, 3, "Prepare a lecture", 1);
                ssd1306_text(0, 4, "in the web app", 1);
            }
            snprintf(line, sizeof line, "Offline: %s %lu min", chunk_store_medium(),
                     (unsigned long)(chunk_store_capacity_s() / 60));
            ssd1306_text(0, 6, line, 1);
#if CONFIG_LC_AUDIO_SOURCE_TEST_CLIP
            ssd1306_text(0, 7, "TEST CLIP, NOT MIC", 1);
#else
            snprintf(line, sizeof line, "Mic %.4s fw %s", g_config.device_id, FIRMWARE_VERSION);
            ssd1306_text(0, 7, line, 1);
#endif
            break;
    }
    /* Warnings override the bottom lines: never drop audio without telling the lecturer. */
    if (g_facts.buffer_full) {
        ssd1306_fill_rect(0, 48, 128, 16, false);
        ssd1306_text(0, 6, "BUFFER FULL", 1);
        ssd1306_text(0, 7, "AUDIO NOT SAVED", 1);
    } else if (g_facts.flash_pct >= 90) {
        ssd1306_fill_rect(0, 48, 128, 16, false);
        ssd1306_text(0, 6, "STORAGE ALMOST FULL", 1);
        ssd1306_text(0, 7, "Restore Wi-Fi now", 1);
    } else if (g_facts.mic_silent) {
        ssd1306_fill_rect(0, 48, 128, 16, false);
        ssd1306_text(0, 6, "NO SPEECH 60 s", 1);
        ssd1306_text(0, 7, "Check microphone", 1);
    }
    if (s_oled_ok && ssd1306_flush() != ESP_OK) ESP_LOGW(TAG, "OLED write failed");
}

static void ui_task(void *arg) {
    (void)arg;
    unsigned tick = 0;
    for (;;) {
        const device_state_t st = app_state_derive();
        led_set(led_for(st, tick));
        if (tick % 2 == 0) draw(st);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void ui_start(void) {
    led_init();
    led_set(OFF);
    s_oled_ok = ssd1306_init(PIN_SDA, PIN_SCL, OLED_ADDR) == ESP_OK;
    if (!s_oled_ok) ESP_LOGE(TAG, "OLED not found at 0x3C on SDA=%d SCL=%d (check VCC/GND order)", PIN_SDA, PIN_SCL);
    xTaskCreatePinnedToCore(ui_task, "ui", 4096, NULL, 2, NULL, 0);
}
