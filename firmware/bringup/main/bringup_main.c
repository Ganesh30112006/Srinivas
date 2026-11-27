/*
 * Bring-up test: checks every part except the microphone, one after another, then keeps showing
 * the buttons live. Results go to the serial monitor and to the OLED.
 *
 *   1. I2C scan and OLED      4. Wi-Fi scan (also loads the 3.3 V supply the way recording will)
 *   2. LEDs, one at a time    5. Buttons, live, forever
 *   3. SD card: mount, write and read back 1 MB, speed
 *
 * Pins come from firmware/board/board.h. Build: idf.py set-target esp32 && idf.py build flash monitor
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ssd1306.h"
#if BOARD_HAS_SD
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "bringup";

static bool s_oled;
static char s_line_oled[32] = "OLED  ?";
static char s_line_sd[32] = "SD    skipped";
static char s_line_wifi[32] = "WiFi  ?";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void oled_show(const char *l0, const char *l1, const char *l2, const char *l3) {
    if (!s_oled) return;
    ssd1306_clear();
    const char *lines[4] = {l0, l1, l2, l3};
    for (int i = 0; i < 4; i++) {
        if (lines[i] != NULL) ssd1306_text(0, i * 2, lines[i], 1);
    }
    ssd1306_flush();
}

/* ── 1. I2C scan + OLED ─────────────────────────────────────────────────── */

static void test_oled(void) {
    printf("\n== 1. Display (I2C on SDA=GPIO%d, SCL=GPIO%d) ==\n", PIN_I2C_SDA, PIN_I2C_SCL);
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    int found = 0;
    if (i2c_new_master_bus(&cfg, &bus) == ESP_OK) {
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
                printf("  I2C device found at 0x%02X%s\n", addr, addr == 0x3C || addr == 0x3D ? " (an SSD1306 display)" : "");
                found++;
            }
        }
        i2c_del_master_bus(bus);
    }
    if (found == 0) {
        printf("  FAIL: nothing answers on I2C.\n"
               "  Check: display VCC on 3.3 V and GND on GND (V1/V2 the right way round),\n"
               "         SDA wire to D21, SCL wire to D22, wires pushed fully into the display's columns.\n");
        snprintf(s_line_oled, sizeof s_line_oled, "OLED  not found");
        return;
    }
    s_oled = ssd1306_init(PIN_I2C_SDA, PIN_I2C_SCL, 0x3C) == ESP_OK;
    if (!s_oled) s_oled = ssd1306_init(PIN_I2C_SDA, PIN_I2C_SCL, 0x3D) == ESP_OK;
    printf("  %s\n", s_oled ? "PASS: display started. It should show BRINGUP." : "FAIL: device found but the display did not start");
    snprintf(s_line_oled, sizeof s_line_oled, "OLED  %s", s_oled ? "OK" : "FAIL");
    if (s_oled) {
        ssd1306_clear();
        ssd1306_text(0, 0, "BRINGUP", 2);
        ssd1306_text(0, 3, BOARD_NAME, 1);
        ssd1306_text(0, 5, "Watch the serial", 1);
        ssd1306_text(0, 6, "monitor too.", 1);
        ssd1306_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

/* ── 2. LEDs ────────────────────────────────────────────────────────────── */

static const int LED_PINS[3] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
static const char *const LED_NAMES[3] = {"red", "green", "third"};

static void leds_set(int r, int g, int b) {
    gpio_set_level(PIN_LED_R, r);
    gpio_set_level(PIN_LED_G, g);
    gpio_set_level(PIN_LED_B, b);
}

static void test_leds(void) {
    printf("\n== 2. LEDs (red=GPIO%d, green=GPIO%d, third=GPIO%d) ==\n", PIN_LED_R, PIN_LED_G, PIN_LED_B);
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(LED_PINS[i]);
        gpio_set_direction(LED_PINS[i], GPIO_MODE_OUTPUT);
    }
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 3; i++) {
            leds_set(i == 0, i == 1, i == 2);
            printf("  GPIO%-2d on: the %s LED should light alone\n", LED_PINS[i], LED_NAMES[i]);
            char l1[32];
            snprintf(l1, sizeof l1, "GPIO%d on", LED_PINS[i]);
            char l2[32];
            snprintf(l2, sizeof l2, "-> %s LED lit?", LED_NAMES[i]);
            oled_show("LED TEST", l1, l2, round == 0 ? "round 1 of 2" : "round 2 of 2");
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
    }
    leds_set(0, 0, 0);
    printf("  CHECK BY EYE: one LED at a time, in the order red, green, third.\n"
           "  - An LED never lights: its long leg faces the wrong way, or its resistor/wire is loose.\n"
           "  - Two light and one goes dark each step: it is a common-anode RGB LED. Tell Claude.\n");
}

/* ── 3. SD card ─────────────────────────────────────────────────────────── */

#if BOARD_HAS_SD
static void test_sd(void) {
    printf("\n== 3. SD card (CS=GPIO%d, SCK=GPIO%d, MISO=GPIO%d, MOSI=GPIO%d) ==\n", PIN_SD_CS, PIN_SD_SCK,
           PIN_SD_MISO, PIN_SD_MOSI);
    oled_show("SD CARD", "mounting...", NULL, NULL);
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
        printf("  FAIL: SPI bus init\n");
        snprintf(s_line_sd, sizeof s_line_sd, "SD    SPI FAIL");
        return;
    }
    /* Pull-ups keep the lines defined over long jumper wires (the SD spec wants them on MISO and CS). */
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS, GPIO_PULLUP_ONLY);
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_SPI_HOST;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    /* Jumper wires are slow: try 10 MHz, then 4 MHz, then the 400 kHz identification speed. */
    static const int SPEEDS_KHZ[] = {10000, 4000, 400};
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof SPEEDS_KHZ / sizeof SPEEDS_KHZ[0]; i++) {
        host.max_freq_khz = SPEEDS_KHZ[i];
        err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mount, &card);
        printf("  at %d kHz: %s\n", SPEEDS_KHZ[i], err == ESP_OK ? "mounted" : esp_err_to_name(err));
        if (err == ESP_OK || err == ESP_FAIL) break; /* ESP_FAIL = card fine, filesystem not */
    }
    if (err != ESP_OK) {
        printf("  FAIL: %s\n", esp_err_to_name(err));
        if (err == ESP_FAIL) {
            printf("  The card answered but has no FAT filesystem: format it as FAT32 on the PC.\n");
            snprintf(s_line_sd, sizeof s_line_sd, "SD    not FAT32");
        } else if (err == ESP_ERR_INVALID_CRC) {
            printf("  The card answers, but its data arrives corrupted. Check: adapter VCC on VIN (5 V),\n"
                   "  not 3.3 V; MISO wire (D19) firmly in; GND; shorter wires if you have them.\n");
            snprintf(s_line_sd, sizeof s_line_sd, "SD    data errors");
        } else {
            printf("  The card did not answer. Check: card pushed in, adapter VCC on VIN (5 V) not 3.3 V,\n"
                   "  GND, and CS->D5, SCK->D18, MISO->D19, MOSI->D23 (MISO and MOSI are easy to swap).\n");
            snprintf(s_line_sd, sizeof s_line_sd, "SD    no card");
        }
        return;
    }
    sdmmc_card_print_info(stdout, card);
    const uint64_t size_mb = (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024);

    static uint8_t block[4096];
    const int blocks = 256; /* 1 MB */
    const char *path = "/sd/lctest.bin";
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  FAIL: cannot create %s (card write-protected or full?)\n", path);
        snprintf(s_line_sd, sizeof s_line_sd, "SD    write FAIL");
        esp_vfs_fat_sdcard_unmount("/sd", card);
        return;
    }
    int64_t t0 = now_ms();
    bool ok = true;
    for (int i = 0; i < blocks && ok; i++) {
        for (size_t j = 0; j < sizeof block; j++) block[j] = (uint8_t)(i * 31 + j);
        ok = fwrite(block, 1, sizeof block, f) == sizeof block;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    const int64_t write_ms = now_ms() - t0;

    f = fopen(path, "rb");
    int bad_blocks = 0;
    t0 = now_ms();
    for (int i = 0; f != NULL && i < blocks; i++) {
        if (fread(block, 1, sizeof block, f) != sizeof block) {
            bad_blocks = blocks - i;
            break;
        }
        for (size_t j = 0; j < sizeof block; j++) {
            if (block[j] != (uint8_t)(i * 31 + j)) {
                bad_blocks++;
                break;
            }
        }
    }
    const int64_t read_ms = now_ms() - t0;
    if (f != NULL) fclose(f);
    unlink(path);
    esp_vfs_fat_sdcard_unmount("/sd", card);

    const unsigned wkb = write_ms > 0 ? (unsigned)(1024 * 1000 / write_ms) : 0;
    const unsigned rkb = read_ms > 0 ? (unsigned)(1024 * 1000 / read_ms) : 0;
    if (ok && f != NULL && bad_blocks == 0) {
        printf("  PASS: %llu MB card, wrote 1 MB at %u KB/s, read back at %u KB/s, data correct.\n"
               "  (The recorder needs about 10 KB/s, so any speed over 100 KB/s is plenty.)\n",
               (unsigned long long)size_mb, wkb, rkb);
        snprintf(s_line_sd, sizeof s_line_sd, "SD    OK %lluMB", (unsigned long long)size_mb);
    } else {
        printf("  FAIL: write %s, %d of %d blocks read back wrong. Loose wire or a bad card.\n", ok ? "ok" : "failed",
               bad_blocks, blocks);
        snprintf(s_line_sd, sizeof s_line_sd, "SD    data FAIL");
    }
}
#endif

/* ── 4. Wi-Fi scan ──────────────────────────────────────────────────────── */

static void test_wifi(void) {
    printf("\n== 4. Wi-Fi scan ==\n");
    oled_show("WIFI", "scanning...", NULL, NULL);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        printf("  FAIL: Wi-Fi did not start\n");
        snprintf(s_line_wifi, sizeof s_line_wifi, "WiFi  FAIL");
        return;
    }
    esp_wifi_scan_start(NULL, true);
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    static wifi_ap_record_t recs[8];
    uint16_t n = 8;
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        printf("  %-32s %4d dBm  channel %d\n", (const char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
    }
    printf("  %s: %u networks seen. Your 2.4 GHz network must be in this list (5 GHz-only networks never are).\n",
           total > 0 ? "PASS" : "FAIL", total);
    snprintf(s_line_wifi, sizeof s_line_wifi, "WiFi  %u networks", total);
}

/* ── 5. Buttons, live ───────────────────────────────────────────────────── */

typedef struct {
    int pin;
    const char *name;
    bool down;
    int64_t since;
    bool warned;
} button_t;

#if BOARD_ONE_BUTTON
static button_t s_buttons[] = {
    {.pin = PIN_BTN_MAIN, .name = "MAIN (D32)"},
    {.pin = PIN_BTN_MAIN2, .name = "BOOT"},
    {.pin = PIN_BTN_STOP, .name = "STOP (D27)"},
};
#else
static button_t s_buttons[] = {
    {.pin = PIN_BTN_START, .name = "START"},
    {.pin = PIN_BTN_STOP, .name = "STOP"},
    {.pin = PIN_BTN_PAUSE, .name = "PAUSE"},
};
#endif
#define N_BUTTONS (sizeof s_buttons / sizeof s_buttons[0])

static void buttons_live(void) {
    printf("\n== 5. Buttons (live) ==\n"
           "  Press each button. Short press and hold (1.5 s+) are both reported.\n");
    for (size_t i = 0; i < N_BUTTONS; i++) {
        gpio_reset_pin(s_buttons[i].pin);
        gpio_set_direction(s_buttons[i].pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_buttons[i].pin, GPIO_PULLUP_ONLY);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("\nSUMMARY  %s | %s | %s | LEDs: check by eye\n\n", s_line_oled, s_line_sd, s_line_wifi);
    int64_t last_draw = 0;
    for (;;) {
        const int64_t now = now_ms();
        bool any_main = false;
        bool any_stop = false;
        for (size_t i = 0; i < N_BUTTONS; i++) {
            button_t *b = &s_buttons[i];
            const bool down = gpio_get_level(b->pin) == 0;
            if (down != b->down) {
                if (now - b->since >= 30) { /* debounce */
                    if (down) {
                        printf("  %s pressed\n", b->name);
                    } else {
                        const int64_t held = now - b->since;
                        printf("  %s released after %lld ms (%s)\n", b->name, (long long)held,
                               held >= 1500 ? "long press = STOP" : "short press");
                        b->warned = false;
                    }
                    b->down = down;
                    b->since = now;
                }
            } else if (down && !b->warned && now - b->since > 8000) {
                printf("  %s has read 'pressed' for 8 s. If you are not pressing it, the button is in the\n"
                       "  wrong way round: pull it out, turn it 90 degrees, push it back in.\n",
                       b->name);
                b->warned = true;
            }
            if (b->down) {
                if (strncmp(b->name, "STOP", 4) == 0) any_stop = true;
                else any_main = true;
            }
        }
        /* LEDs follow the buttons: green idle, red while MAIN/BOOT is down, third while STOP is down. */
        leds_set(any_main, !any_main && !any_stop, any_stop);
        if (now - last_draw >= 200) {
            last_draw = now;
            char l3[32];
            snprintf(l3, sizeof l3, "%s%s%s", s_buttons[0].down ? "MAIN " : "", s_buttons[1].down ? "BOOT " : "",
                     s_buttons[2].down ? "STOP" : "");
            if (l3[0] == '\0') snprintf(l3, sizeof l3, "Press a button");
            oled_show(s_line_oled, s_line_sd, s_line_wifi, l3);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    printf("\n\n===== Lecture recorder bring-up test =====\n"
           "Board %s, %d cores, rev %d.%d, %lu MB flash, free RAM %lu KB\n",
           BOARD_NAME, chip.cores, chip.revision / 100, chip.revision % 100, (unsigned long)(flash >> 20),
           (unsigned long)(esp_get_free_heap_size() / 1024));
    (void)TAG;
    test_oled();
    test_leds();
#if BOARD_HAS_SD
    test_sd();
#endif
    test_wifi();
    buttons_live();
}
