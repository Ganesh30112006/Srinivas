/*
 * Board pins, one block per supported board. The build target picks the block:
 *   idf.py set-target esp32     -> ESP32 DevKit V1 (ESP32-WROOM-32, 30 pins), the recorder as built
 *   idf.py set-target esp32s3   -> ESP32-S3-DevKitC-1 N8R8, the original design
 * firmware/README.md §1 and docs/project/CIRCUIT.md list the same pins; this file wins if they differ.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

#define BOARD_NAME "ESP32 DevKit V1"

/* INMP441 microphone (I2S) */
#define PIN_I2S_BCLK 26
#define PIN_I2S_WS 25
#define PIN_I2S_DIN 33

/* SSD1306 OLED (I2C) */
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

/* MicroSD adapter (SPI, VSPI pins). The adapter has its own 3.3 V regulator: VCC goes to VIN (5 V). */
#define BOARD_HAS_SD 1
#define SD_SPI_HOST SPI3_HOST
#define PIN_SD_SCK 18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_SD_CS 5

/* Status LED: one RGB LED, or three single-colour LEDs (red, green, blue or yellow). */
#define PIN_LED_R 16
#define PIN_LED_G 17
#define PIN_LED_B 4

/*
 * Buttons, all to GND with internal pull-ups. One main button does everything:
 * short press = start / pause / resume, long press = stop. The board's BOOT button (GPIO0) is a
 * second main button, so the device works with no buttons wired at all. GPIO27 is an optional
 * dedicated Stop button.
 */
#define PIN_BTN_MAIN 32
#define PIN_BTN_MAIN2 0
#define PIN_BTN_STOP 27
#define BOARD_ONE_BUTTON 1
#define BOARD_RESUME_HINT "Press to resume"

#elif CONFIG_IDF_TARGET_ESP32S3

#define BOARD_NAME "ESP32-S3-DevKitC-1"

#define PIN_I2S_BCLK 4
#define PIN_I2S_WS 5
#define PIN_I2S_DIN 6

#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9

#define BOARD_HAS_SD 0

#define PIN_LED_R 16
#define PIN_LED_G 17
#define PIN_LED_B 18

/* Three buttons: Start, Stop, Pause/Resume. */
#define PIN_BTN_START 1
#define PIN_BTN_STOP 2
#define PIN_BTN_PAUSE 7
#define BOARD_ONE_BUTTON 0
#define BOARD_RESUME_HINT "Press Pause to resume"

#else
#error "Unsupported target: run idf.py set-target esp32 (or esp32s3)"
#endif

/* Large buffers go to PSRAM when the board has it, otherwise to internal RAM. */
#if CONFIG_SPIRAM
#define BOARD_BIG_CAPS MALLOC_CAP_SPIRAM
#else
#define BOARD_BIG_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
