#include "buttons.h"

#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "session.h"
#include "wifi_setup.h"

#define SETTLE_MS 30
#define REPEAT_GUARD_MS 400
#define LONG_PRESS_MS 1500
#define SETUP_PRESS_MS 5000 /* while not recording: restart into Wi-Fi setup */

typedef enum { ROLE_MAIN, ROLE_START, ROLE_STOP, ROLE_PAUSE } role_t;

typedef struct {
    int pin;
    role_t role;
} button_t;

static const button_t BUTTONS[] = {
#if BOARD_ONE_BUTTON
    {PIN_BTN_MAIN, ROLE_MAIN},
    {PIN_BTN_MAIN2, ROLE_MAIN},
    {PIN_BTN_STOP, ROLE_STOP},
#else
    {PIN_BTN_START, ROLE_START},
    {PIN_BTN_STOP, ROLE_STOP},
    {PIN_BTN_PAUSE, ROLE_PAUSE},
#endif
};
#define N_BUTTONS (sizeof BUTTONS / sizeof BUTTONS[0])

static const char *TAG = "buttons";
static QueueHandle_t s_events;

static void IRAM_ATTR on_edge(void *arg) {
    const uint32_t index = (uint32_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_events, &index, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static bool recording(void) {
    const session_info_t s = session_info();
    return s.active && !s.stopped;
}

static void on_start(void) {
    char lecture[37] = "";
    app_get_assigned(lecture, sizeof lecture, NULL, 0);
    if (g_facts.buffer_full) {
        ESP_LOGW(TAG, "start ignored: buffers full");
        return;
    }
    if (session_start(lecture)) ESP_LOGI(TAG, "recording started");
}

static void on_pause(void) {
    const session_info_t s = session_info();
    if (!s.active || s.stopped) {
        ESP_LOGI(TAG, "pause ignored: not recording");
        return;
    }
    session_set_paused(!s.paused); /* the net task tells the server */
    ESP_LOGI(TAG, "%s", s.paused ? "resumed" : "paused");
}

static void on_stop(void) {
    if (!recording()) return;
    session_stop();
    ESP_LOGI(TAG, "recording stopped");
}

/* Main button: acts on release for a short press, or as soon as the hold reaches LONG_PRESS_MS.
 * While not recording, a 5 s hold restarts into Wi-Fi setup instead. */
static void on_main(int pin) {
    const uint32_t down_at = app_now_ms();
    const bool was_recording = recording();
    while (gpio_get_level((gpio_num_t)pin) == 0) {
        const uint32_t held = app_now_ms() - down_at;
        if (was_recording && held >= LONG_PRESS_MS) {
            on_stop();
            while (gpio_get_level((gpio_num_t)pin) == 0) vTaskDelay(pdMS_TO_TICKS(20)); /* wait for release */
            return;
        }
        if (!was_recording && held >= SETUP_PRESS_MS) {
            ESP_LOGI(TAG, "held 5 s while idle: restarting into Wi-Fi setup");
            wifi_setup_request_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!was_recording && app_now_ms() - down_at >= LONG_PRESS_MS) return; /* a hold that was let go early */
    if (recording()) on_pause();
    else on_start();
}

static void button_task(void *arg) {
    (void)arg;
    uint32_t last_accept[N_BUTTONS] = {0};
    uint32_t index;
    for (;;) {
        if (xQueueReceive(s_events, &index, portMAX_DELAY) != pdTRUE || index >= N_BUTTONS) continue;
        const button_t *b = &BUTTONS[index];
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        if (gpio_get_level((gpio_num_t)b->pin) != 0) continue; /* bounce or glitch */
        const uint32_t now = app_now_ms();
        if (now - last_accept[index] < REPEAT_GUARD_MS) continue;
        switch (b->role) {
            case ROLE_MAIN: on_main(b->pin); break;
            case ROLE_START: on_start(); break;
            case ROLE_PAUSE: on_pause(); break;
            case ROLE_STOP: on_stop(); break;
        }
        last_accept[index] = app_now_ms();
        xQueueReset(s_events); /* drop edges queued while a long press was being timed */
    }
}

void buttons_start(void) {
    s_events = xQueueCreate(8, sizeof(uint32_t));
    uint64_t mask = 0;
    for (size_t i = 0; i < N_BUTTONS; i++) mask |= 1ULL << BUTTONS[i].pin;
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    for (size_t i = 0; i < N_BUTTONS; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTONS[i].pin, on_edge, (void *)(uint32_t)i));
    }
    xTaskCreatePinnedToCore(button_task, "buttons", 3072, NULL, 6, NULL, 0);
}
