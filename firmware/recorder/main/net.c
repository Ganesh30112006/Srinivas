#include "net.h"

#include <string.h>
#include <sys/time.h>

#include "app.h"
#include "board.h"
#include "cJSON.h"
#include "chunk_store.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mqtt_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "lc_backoff.h"
#include "session.h"

#define BIT_WIFI BIT0
#define BIT_WS_UP BIT1
#define BIT_WS_DOWN BIT2
#define RX_QUEUE_LEN 16
#define MAX_TEXT 4096
#define STATUS_PERIOD_MS 5000
#define STATUS_PERIOD_RECORDING_MS 1000 /* drives the web Live page's level meter */
#define STOP_RETRY_MS 10000
#define DECL_RETRY_MS 5000 /* resend start/resume if the server hasn't confirmed it */

static const char *TAG = "net";
static EventGroupHandle_t s_events;
static QueueHandle_t s_rx;
static esp_websocket_client_handle_t s_ws;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_use_iot;
static char s_up_topic[64];
static char s_down_topic[64];
static esp_timer_handle_t s_wifi_retry;
static uint32_t s_wifi_attempt;
static int s_wifi_index; /* which saved network is being tried */
static volatile uint8_t s_wifi_reason; /* last disconnect reason (0 = none yet) */
static volatile int s_close_code;
static volatile bool s_link_reset; /* the server asked for a reconnect (MQTT `close`) */
static char *s_accum;
static size_t s_accum_len;
/* The session the server last confirmed with `lecture.started` (frames wait for it). */
static uint8_t s_confirmed[16];
static volatile bool s_confirmed_valid;

/* ── Wi-Fi + SNTP ───────────────────────────────────────────────────────── */

static void time_synced(struct timeval *tv) {
    (void)tv;
    g_facts.time_synced = true;
    ESP_LOGI(TAG, "SNTP time synced");
}

const char *net_wifi_problem(void) {
    switch (s_wifi_reason) {
        case 0: return "Connecting...";
        case WIFI_REASON_NO_AP_FOUND: return "Network not found";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Wrong password?";
        default: return "Connection lost";
    }
}

const char *net_wifi_ssid(void) {
    return g_config.wifi_count > 0 ? g_config.wifi[s_wifi_index].ssid : "";
}

static void wifi_apply(int index) {
    if (g_config.wifi_count == 0) return;
    s_wifi_index = index % g_config.wifi_count;
    const wifi_network_t *n = &g_config.wifi[s_wifi_index];
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, n->ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, n->pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = n->pass[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true; /* phone hotspots may use WPA2/WPA3 mixed mode */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void wifi_retry_cb(void *arg) {
    (void)arg;
    /* Try the next saved network on each attempt; the backoff grows once all have failed. */
    if (g_config.wifi_count > 1) wifi_apply(s_wifi_index + 1);
    esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_wifi_reason = d->reason;
        ESP_LOGW(TAG, "Wi-Fi %s: %s (reason %u)", net_wifi_ssid(), net_wifi_problem(), d->reason);
        g_facts.wifi_up = false;
        xEventGroupClearBits(s_events, BIT_WIFI);
        const uint32_t round = s_wifi_attempt++ / (uint32_t)(g_config.wifi_count > 0 ? g_config.wifi_count : 1);
        const uint32_t delay = lc_backoff_delay_ms(round, esp_random()) + 250;
        esp_timer_start_once(s_wifi_retry, (uint64_t)delay * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_attempt = 0;
        s_wifi_reason = 0;
        ESP_LOGI(TAG, "Wi-Fi %s up", net_wifi_ssid());
        g_facts.wifi_up = true;
        xEventGroupSetBits(s_events, BIT_WIFI);
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    const esp_timer_create_args_t retry = {.callback = wifi_retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&retry, &s_wifi_retry));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_apply(0);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); /* steady uplink beats power saving here */

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = time_synced;
    esp_netif_sntp_init(&sntp);
}

/* ── WebSocket plumbing ─────────────────────────────────────────────────── */

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *d = event_data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupClearBits(s_events, BIT_WS_DOWN);
            xEventGroupSetBits(s_events, BIT_WS_UP);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            xEventGroupClearBits(s_events, BIT_WS_UP);
            xEventGroupSetBits(s_events, BIT_WS_DOWN);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (d->op_code == 0x08 && d->data_len >= 2) {
                s_close_code = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
                break;
            }
            if (d->op_code != 0x01 && d->op_code != 0x00) break; /* text (or its continuation) only */
            if (d->payload_len <= 0 || d->payload_len > MAX_TEXT) break;
            if (d->payload_offset == 0) {
                free(s_accum);
                s_accum = malloc((size_t)d->payload_len + 1);
                s_accum_len = 0;
            }
            if (s_accum == NULL || d->payload_offset + d->data_len > d->payload_len) break;
            memcpy(s_accum + d->payload_offset, d->data_ptr, (size_t)d->data_len);
            s_accum_len = (size_t)(d->payload_offset + d->data_len);
            if (s_accum_len == (size_t)d->payload_len) {
                s_accum[s_accum_len] = '\0';
                if (xQueueSend(s_rx, &s_accum, 0) != pdTRUE) free(s_accum);
                s_accum = NULL;
            }
            break;
        default:
            break;
    }
}

/* ── MQTT (AWS IoT Core) plumbing ───────────────────────────────────────── */

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t e = event_data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            esp_mqtt_client_subscribe(s_mqtt, s_down_topic, 1);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            /* SUBACK 0x80 = refused by the IoT policy. */
            if (e->data_len > 0 && (uint8_t)e->data[0] == 0x80) {
                ESP_LOGE(TAG, "subscribe to %s refused (check the IoT policy)", s_down_topic);
                xEventGroupSetBits(s_events, BIT_WS_DOWN);
                break;
            }
            xEventGroupClearBits(s_events, BIT_WS_DOWN);
            xEventGroupSetBits(s_events, BIT_WS_UP);
            break;
        case MQTT_EVENT_DISCONNECTED:
        case MQTT_EVENT_ERROR:
            xEventGroupClearBits(s_events, BIT_WS_UP);
            xEventGroupSetBits(s_events, BIT_WS_DOWN);
            break;
        case MQTT_EVENT_DATA:
            if (e->total_data_len <= 0 || e->total_data_len > MAX_TEXT) break;
            if (e->current_data_offset == 0) {
                free(s_accum);
                s_accum = malloc((size_t)e->total_data_len + 1);
                s_accum_len = 0;
            }
            if (s_accum == NULL || e->current_data_offset + e->data_len > e->total_data_len) break;
            memcpy(s_accum + e->current_data_offset, e->data, (size_t)e->data_len);
            s_accum_len = (size_t)(e->current_data_offset + e->data_len);
            if (s_accum_len == (size_t)e->total_data_len) {
                s_accum[s_accum_len] = '\0';
                if (xQueueSend(s_rx, &s_accum, 0) != pdTRUE) free(s_accum);
                s_accum = NULL;
            }
            break;
        default:
            break;
    }
}

static bool mqtt_open(void) {
    static char uri[160];
    snprintf(uri, sizeof uri, "mqtts://%s:8883", g_config.iot_endpoint);
    snprintf(s_up_topic, sizeof s_up_topic, "lc/%s/up", g_config.device_id);
    snprintf(s_down_topic, sizeof s_down_topic, "lc/%s/down", g_config.device_id);
    static const char BYE[] = "{\"type\":\"bye\"}";
    const esp_mqtt_client_config_t cfg = {
        .broker = {.address = {.uri = uri}, .verification = {.crt_bundle_attach = esp_crt_bundle_attach}},
        .credentials =
            {
                .client_id = g_config.device_id, /* = the IoT thing name; the policy checks it */
                .authentication = {.certificate = g_config.iot_cert, .key = g_config.iot_key},
            },
        .session =
            {
                .keepalive = 30,
                /* IoT Core publishes this if the device drops without saying goodbye. */
                .last_will = {.topic = s_up_topic, .msg = BYE, .msg_len = sizeof BYE - 1, .qos = 1},
            },
        .network = {.timeout_ms = 10000, .disable_auto_reconnect = true},
        .buffer = {.size = 2048, .out_size = CHUNK_FRAME_MAX + 256},
        .task = {.stack_size = 6144},
    };
    s_close_code = 0;
    s_link_reset = false;
    xEventGroupClearBits(s_events, BIT_WS_UP | BIT_WS_DOWN);
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (s_mqtt == NULL) return false;
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) return false;
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, BIT_WS_UP | BIT_WS_DOWN, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & BIT_WS_UP) != 0;
}

static void mqtt_close(void) {
    if (s_mqtt == NULL) return;
    esp_mqtt_client_stop(s_mqtt);
    esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = NULL;
}

/* ── WebSocket plumbing ─────────────────────────────────────────────────── */

static bool ws_open(void) {
    esp_websocket_client_config_t cfg = {0};
    cfg.uri = g_config.server_url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_reconnect = true;
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 10;
    cfg.pingpong_timeout_sec = 30;
    s_close_code = 0;
    xEventGroupClearBits(s_events, BIT_WS_UP | BIT_WS_DOWN);
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) return false;
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, BIT_WS_UP | BIT_WS_DOWN, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & BIT_WS_UP) != 0;
}

static void ws_close(void) {
    if (s_ws == NULL) return;
    if (esp_websocket_client_is_connected(s_ws)) esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

/* ── transport: one of the two links above ──────────────────────────────── */

static bool link_open(void) {
    s_confirmed_valid = false;
    return s_use_iot ? mqtt_open() : ws_open();
}

static void link_close(void) {
    if (s_use_iot) mqtt_close();
    else ws_close();
    char *msg = NULL;
    while (xQueueReceive(s_rx, &msg, 0) == pdTRUE) free(msg);
}

static bool connected(void) {
    const bool open = s_use_iot ? s_mqtt != NULL : s_ws != NULL;
    return open && !s_link_reset && (xEventGroupGetBits(s_events) & BIT_WS_UP) != 0;
}

static bool link_send(const void *data, size_t len, bool binary) {
    if (s_use_iot) {
        /* Control messages QoS 1; audio QoS 0 (the protocol's own ACKs make it reliable). */
        return esp_mqtt_client_publish(s_mqtt, s_up_topic, data, (int)len, binary ? 0 : 1, 0) >= 0;
    }
    return binary ? esp_websocket_client_send_bin(s_ws, data, (int)len, pdMS_TO_TICKS(5000)) >= 0
                  : esp_websocket_client_send_text(s_ws, data, (int)len, pdMS_TO_TICKS(5000)) >= 0;
}

static bool send_json(cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) return false;
    const bool sent = link_send(text, strlen(text), false);
    free(text);
    return sent;
}

/* ── protocol messages ──────────────────────────────────────────────────── */

static bool send_hello(void) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "hello");
    cJSON_AddStringToObject(m, "device_id", g_config.device_id);
    cJSON_AddStringToObject(m, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(m, "token", g_config.device_token);
    cJSON_AddNumberToObject(m, "protocol_version", 1);
    return send_json(m);
}

static bool send_session_decl(const char *type, const uint8_t id[16], bool lecture_known, const char *lecture,
                              uint64_t started_unix_ms) {
    char sid[37];
    uuid_to_str(id, sid);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", type);
    cJSON_AddStringToObject(m, "session_id", sid);
    if (lecture_known) cJSON_AddStringToObject(m, "lecture_id", lecture);
    else cJSON_AddNullToObject(m, "lecture_id");
    if (strcmp(type, "start") == 0) {
        cJSON_AddNumberToObject(m, "started_unix_ms", (double)started_unix_ms);
        cJSON_AddNumberToObject(m, "sample_rate", LC_SAMPLE_RATE);
    }
    return send_json(m);
}

static bool send_stop(const uint8_t id[16], int32_t last_seq) {
    char sid[37];
    uuid_to_str(id, sid);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "stop");
    cJSON_AddStringToObject(m, "session_id", sid);
    cJSON_AddNumberToObject(m, "last_seq", last_seq);
    return send_json(m);
}

static bool send_status(device_state_t state) {
    wifi_ap_record_t ap;
    int rssi = -127;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    g_facts.wifi_rssi = (int8_t)rssi;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "status");
    cJSON_AddNumberToObject(m, "wifi_rssi", rssi > 0 ? 0 : rssi);
    cJSON_AddNumberToObject(m, "psram_buffer_pct", g_facts.psram_pct);
    cJSON_AddNumberToObject(m, "flash_buffer_pct", g_facts.flash_pct);
    cJSON_AddNumberToObject(m, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    const session_info_t s = session_info();
    cJSON_AddBoolToObject(m, "recording", s.active && !s.stopped);
    cJSON_AddStringToObject(m, "state", app_state_name(state));
    cJSON_AddNumberToObject(m, "pending_chunks", g_facts.pending_all);
    /* level_db10 is tenths of dB above -90 dBFS (0..900). */
    cJSON_AddNumberToObject(m, "level_dbfs", (double)g_facts.level_db10 / 10.0 - 90.0);
    if (s.active && !s.stopped) cJSON_AddBoolToObject(m, "mic_ok", !g_facts.mic_silent);
    cJSON_AddBoolToObject(m, "paused", s.active && !s.stopped && s.paused);
    return send_json(m);
}

static bool send_pause(const uint8_t id[16], bool paused) {
    char sid[37];
    uuid_to_str(id, sid);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "pause");
    cJSON_AddStringToObject(m, "session_id", sid);
    cJSON_AddBoolToObject(m, "paused", paused);
    return send_json(m);
}

/* Web Live page buttons (`command`): same effect as the device's own buttons, but only for the
 * lecture they name, so a stale command can't stop a different lecture. */
static void handle_command(const char *action, const char *lecture_id) {
    const session_info_t cur = session_info();
    const bool recording = cur.active && !cur.stopped;
    const bool same_lecture = cur.lecture_known && strcmp(cur.lecture_str, lecture_id) == 0;
    if (strcmp(action, "start") == 0) {
        if (recording || g_facts.buffer_full) {
            ESP_LOGW(TAG, "start command ignored (%s)", recording ? "already recording" : "buffers full");
            return;
        }
        session_start(lecture_id);
    } else if (!recording || !same_lecture) {
        ESP_LOGW(TAG, "%s command ignored: not recording lecture %s", action, lecture_id);
    } else if (strcmp(action, "pause") == 0) {
        session_set_paused(true);
    } else if (strcmp(action, "resume") == 0) {
        session_set_paused(false);
    } else if (strcmp(action, "stop") == 0) {
        session_stop();
    }
}

/* Pending `stop` awaiting the server's ack (one at a time). */
static struct {
    bool waiting;
    uint8_t id[16];
    uint32_t sent_at;
} s_stop;

static void handle_message(const char *text) {
    cJSON *m = cJSON_Parse(text);
    if (m == NULL) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(m, "type");
    if (!cJSON_IsString(type)) goto done;
    if (strcmp(type->valuestring, "config") == 0) {
        const cJSON *gain = cJSON_GetObjectItemCaseSensitive(m, "gain");
        const cJSON *vad = cJSON_GetObjectItemCaseSensitive(m, "vad_enabled");
        const cJSON *srv = cJSON_GetObjectItemCaseSensitive(m, "server_unix_ms");
        if (cJSON_IsNumber(gain) && gain->valuedouble >= 0.5 && gain->valuedouble <= 16) g_facts.gain = (float)gain->valuedouble;
        if (cJSON_IsBool(vad)) g_facts.vad_enabled = cJSON_IsTrue(vad);
        if (cJSON_IsNumber(srv) && !g_facts.time_synced && srv->valuedouble > 1.6e12) {
            /* SNTP blocked on the campus network: fall back to the server's clock. */
            const int64_t ms = (int64_t)srv->valuedouble;
            const struct timeval tv = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000)};
            settimeofday(&tv, NULL);
            g_facts.time_synced = true;
        }
    } else if (strcmp(type->valuestring, "ack") == 0) {
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(m, "session_id");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(m, "seq");
        const cJSON *ranges = cJSON_GetObjectItemCaseSensitive(m, "ranges");
        uint8_t id[16];
        if (!cJSON_IsString(sid) || !uuid_parse(sid->valuestring, id) || !cJSON_IsNumber(seq)) goto done;
        lc_ack_t ack;
        lc_ack_reset(&ack);
        ack.cumulative = (int64_t)seq->valuedouble;
        const int n = cJSON_IsArray(ranges) ? cJSON_GetArraySize(ranges) : 0;
        for (int i = 0; i < n; i++) {
            const cJSON *r = cJSON_GetArrayItem(ranges, i);
            const cJSON *lo = cJSON_GetArrayItem(r, 0);
            const cJSON *hi = cJSON_GetArrayItem(r, 1);
            if (cJSON_IsNumber(lo) && cJSON_IsNumber(hi)) lc_ack_add_range(&ack, (uint32_t)lo->valuedouble, (uint32_t)hi->valuedouble);
        }
        chunk_store_apply_ack(id, &ack);
        if (s_stop.waiting && memcmp(s_stop.id, id, 16) == 0) {
            session_confirm_stopped(id); /* the server answers `stop` with an ack for that session */
            s_stop.waiting = false;
        }
    } else if (strcmp(type->valuestring, "lecture.assign") == 0) {
        const cJSON *lid = cJSON_GetObjectItemCaseSensitive(m, "lecture_id");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(m, "title");
        if (cJSON_IsString(lid) && cJSON_IsString(title)) app_set_assigned(lid->valuestring, title->valuestring);
    } else if (strcmp(type->valuestring, "lecture.started") == 0) {
        const cJSON *lid = cJSON_GetObjectItemCaseSensitive(m, "lecture_id");
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(m, "session_id");
        uint8_t id[16];
        const session_info_t cur = session_info();
        if (cJSON_IsString(sid) && uuid_parse(sid->valuestring, id)) {
            memcpy(s_confirmed, id, 16);
            s_confirmed_valid = true;
        }
        if (cJSON_IsString(lid) && cJSON_IsString(sid) && uuid_parse(sid->valuestring, id) && cur.active &&
            memcmp(cur.id, id, 16) == 0) {
            session_set_lecture(lid->valuestring);
            app_clear_assigned(); /* the prepared lecture is now in use */
        }
    } else if (strcmp(type->valuestring, "command") == 0) {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(m, "action");
        const cJSON *lid = cJSON_GetObjectItemCaseSensitive(m, "lecture_id");
        if (cJSON_IsString(action) && cJSON_IsString(lid) && strlen(lid->valuestring) == 36) {
            handle_command(action->valuestring, lid->valuestring);
        }
    } else if (strcmp(type->valuestring, "close") == 0) {
        /* MQTT has no close frame: the bridge sends the WebSocket close code as a message. */
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(m, "code");
        s_close_code = cJSON_IsNumber(code) ? code->valueint : 4000;
        s_link_reset = true;
        ESP_LOGW(TAG, "server asked to reconnect (code %d)", s_close_code);
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(m, "code");
        if (cJSON_IsString(code)) {
            ESP_LOGW(TAG, "server error: %s", code->valuestring);
            if (strcmp(code->valuestring, "auth_failed") == 0) {
                g_facts.fatal = true;
                app_set_error("Device not authorised");
            } else if (strcmp(code->valuestring, "consent_required") == 0) {
                app_set_error("Confirm consent in web app");
            }
        }
    }
done:
    cJSON_Delete(m);
}

static bool wait_config(void) {
    const uint32_t deadline = app_now_ms() + 10000;
    char *msg = NULL;
    while (app_now_ms() < deadline && connected()) {
        if (xQueueReceive(s_rx, &msg, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        const bool is_config = strstr(msg, "\"config\"") != NULL;
        handle_message(msg);
        free(msg);
        if (is_config) return true;
        if (g_facts.fatal || s_link_reset) break;
    }
    if (s_close_code == 4401) {
        g_facts.fatal = true;
        app_set_error("Device not authorised");
    }
    return false;
}

/* ── main session loop (one connection) ─────────────────────────────────── */

static void run_connection(uint8_t *frame) {
    bool declared = false;
    uint8_t declared_id[16] = {0};
    uint32_t declared_at = 0;
    uint32_t last_status = 0;
    bool pause_reported = false; /* the server knows the current session's pause state */
    bool reported_paused = false;
    char *msg = NULL;
    while (connected()) {
        while (xQueueReceive(s_rx, &msg, 0) == pdTRUE) {
            handle_message(msg);
            free(msg);
        }
        const uint32_t now = app_now_ms();
        const session_info_t live = session_info();
        const uint32_t period = live.active && !live.stopped ? STATUS_PERIOD_RECORDING_MS : STATUS_PERIOD_MS;
        if (now - last_status >= period) {
            if (!send_status(app_state_derive())) break;
            last_status = now;
        }

        /*
         * Whose chunks go next (one session per connection at a time):
         *  1. the current session (recording, finalising, or waiting for its stop to be confirmed);
         *  2. otherwise the oldest earlier session that still has chunks in the flash log;
         *  3. otherwise the previous session, whose `stop` still needs confirming.
         */
        const session_info_t cur = session_info();
        const session_info_t prev = session_previous();
        uint8_t target[16] = {0};
        bool target_known = false;
        char target_lecture[37] = "";
        bool have_target = false;
        bool is_current = false;
        if (cur.active) {
            memcpy(target, cur.id, 16);
            target_known = cur.lecture_known;
            strlcpy(target_lecture, cur.lecture_str, sizeof target_lecture);
            have_target = is_current = true;
        } else if (chunk_store_other_session(NULL, target, target_lecture, &target_known)) {
            have_target = true;
        } else if (prev.active) {
            memcpy(target, prev.id, 16);
            target_known = prev.lecture_known;
            strlcpy(target_lecture, prev.lecture_str, sizeof target_lecture);
            have_target = true;
        }

        const bool confirmed = declared && s_confirmed_valid && memcmp(s_confirmed, declared_id, 16) == 0;
        const bool retry_decl = declared && !confirmed && now - declared_at > DECL_RETRY_MS;
        if (have_target && (!declared || retry_decl || memcmp(declared_id, target, 16) != 0)) {
            const bool ok = is_current ? send_session_decl("start", target, target_known, target_lecture, cur.started_unix_ms)
                                       : send_session_decl("resume", target, target_known, target_lecture, 0);
            if (!ok) break;
            declared = true;
            declared_at = now;
            memcpy(declared_id, target, 16);
            pause_reported = false;
        }

        /* Tell the server whenever the pause state differs from what it last heard (and once per
         * connection, since a pause or resume may have happened while offline). */
        const bool session_ok = declared && s_confirmed_valid && memcmp(s_confirmed, declared_id, 16) == 0;
        if (is_current && session_ok && !cur.stopped && (!pause_reported || reported_paused != cur.paused)) {
            if (!send_pause(cur.id, cur.paused)) break;
            pause_reported = true;
            reported_paused = cur.paused;
        }

        bool sent_frame = false;
        /* Frames only once the server has confirmed the session: over MQTT, messages can arrive
         * out of order, and a frame before its start/resume would be rejected. */
        const bool ready = declared && s_confirmed_valid && memcmp(s_confirmed, declared_id, 16) == 0 &&
                           memcmp(declared_id, target, 16) == 0;
        if (have_target && ready && chunk_store_in_flight() < CONFIG_LC_MAX_IN_FLIGHT) {
            chunk_ref_t ref;
            const size_t len = chunk_store_next_frame(target, frame, CHUNK_FRAME_MAX, &ref);
            if (len > 0) {
                if (!link_send(frame, len, true)) break;
                chunk_store_mark_sent(&ref);
                sent_frame = true;
            }
        }

        /* Stop: only once everything the device holds for the session is acknowledged (§1.4.7). */
        const session_info_t *stopped = NULL;
        if (cur.active && cur.stopped && memcmp(target, cur.id, 16) == 0) stopped = &cur;
        else if (prev.active && memcmp(target, prev.id, 16) == 0) stopped = &prev;
        if (have_target && ready && stopped != NULL && chunk_store_pending(stopped->id) == 0 &&
            (!s_stop.waiting || now - s_stop.sent_at > STOP_RETRY_MS)) {
            if (!send_stop(stopped->id, stopped->last_seq)) break;
            s_stop.waiting = true;
            memcpy(s_stop.id, stopped->id, 16);
            s_stop.sent_at = now;
        }

        if (!sent_frame && xQueueReceive(s_rx, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_message(msg);
            free(msg);
        }
    }
}

static void net_task(void *arg) {
    (void)arg;
    uint8_t *frame = heap_caps_malloc(CHUNK_FRAME_MAX, BOARD_BIG_CAPS);
    configASSERT(frame != NULL);
    wifi_start();
    uint32_t attempt = 0;
    for (;;) {
        xEventGroupWaitBits(s_events, BIT_WIFI, pdFALSE, pdTRUE, portMAX_DELAY);
        if (g_facts.fatal) {
            vTaskDelay(pdMS_TO_TICKS(60000)); /* auth failure: retry slowly in case it was re-provisioned */
            g_facts.fatal = false;
            app_set_error("");
        }
        if (link_open() && send_hello() && wait_config()) {
            attempt = 0;
            g_facts.server_up = true;
            app_set_error("");
            ESP_LOGI(TAG, "connected to %s", s_use_iot ? g_config.iot_endpoint : g_config.server_url);
            run_connection(frame);
            ESP_LOGW(TAG, "connection lost");
        }
        g_facts.server_up = false;
        s_stop.waiting = false;
        chunk_store_requeue();
        link_close();
        const uint32_t delay = lc_backoff_delay_ms(attempt++, esp_random());
        vTaskDelay(pdMS_TO_TICKS(delay + 200));
    }
}

void net_start(void) {
    s_events = xEventGroupCreate();
    s_rx = xQueueCreate(RX_QUEUE_LEN, sizeof(char *));
    s_use_iot = config_uses_iot(&g_config);
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 5, NULL, 0);
}
