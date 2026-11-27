#include "wifi_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SETUP_MAGIC 0x57494649u /* "WIFI" */
#define HOLD_AT_BOOT_MS 1500
#define SETUP_TIMEOUT_US (10LL * 60 * 1000000)
#define MAX_SCAN 20
#define MAX_BODY 512

static const char *TAG = "wifi_setup";
static RTC_NOINIT_ATTR uint32_t s_restart_magic; /* survives esp_restart, not power-off */
static volatile bool s_active;
static char s_ap_name[24];
static char s_ap_pass[12];
static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_count;

bool wifi_setup_active(void) { return s_active; }
const char *wifi_setup_ap_name(void) { return s_ap_name; }
const char *wifi_setup_ap_pass(void) { return s_ap_pass; }

void wifi_setup_request_restart(void) {
    s_restart_magic = SETUP_MAGIC;
    esp_restart();
}

bool wifi_setup_requested(void) {
    const bool requested = s_restart_magic == SETUP_MAGIC && esp_reset_reason() == ESP_RST_SW;
    s_restart_magic = 0;
    if (requested) return true;
    /* Main button (not BOOT: GPIO0 held at reset selects the ROM bootloader) held while starting. */
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BTN_MAIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int waited = 0; waited < HOLD_AT_BOOT_MS; waited += 50) {
        if (gpio_get_level(PIN_BTN_MAIN) != 0) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

/* ── HTML ───────────────────────────────────────────────────────────────── */

static void html_escape(const char *in, char *out, size_t len) {
    size_t o = 0;
    for (; *in != '\0' && o + 7 < len; in++) {
        const char *rep = NULL;
        switch (*in) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep != NULL) {
            strcpy(out + o, rep);
            o += strlen(rep);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Recorder Wi-Fi</title>"
    "<style>body{font:16px system-ui,sans-serif;margin:0;padding:16px;max-width:30em;color:#1b1f24;background:#fff}"
    "h1{font-size:1.3em}label{display:block;margin:14px 0 4px;font-weight:600}"
    "input,select{width:100%;box-sizing:border-box;font:inherit;padding:10px;border:1px solid #9aa3ad;border-radius:6px}"
    "button{margin-top:18px;width:100%;padding:12px;font:inherit;font-weight:600;border:0;border-radius:6px;"
    "background:#1d4ed8;color:#fff}.note{color:#5d6670;font-size:.9em}"
    "li{margin:4px 0}.cb{display:flex;gap:8px;align-items:center;font-weight:400}.cb input{width:auto}</style>"
    "</head><body>";

static esp_err_t get_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Recorder Wi-Fi</h1>"
                                  "<p class=note>Pick the network the recorder should use. It must be 2.4 GHz "
                                  "(on an iPhone hotspot, turn on Maximize Compatibility).</p>"
                                  "<form method=post action=/save><label for=pick>Networks in range</label>"
                                  "<select id=pick onchange=\"document.getElementById('ssid').value=this.value\">"
                                  "<option value=''>Choose…</option>");
    char esc[100], line[320];
    for (uint16_t i = 0; i < s_scan_count; i++) {
        html_escape((const char *)s_scan[i].ssid, esc, sizeof esc);
        if (esc[0] == '\0') continue;
        snprintf(line, sizeof line, "<option value=\"%s\">%s (%d dBm)</option>", esc, esc, s_scan[i].rssi);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "</select>"
                                  "<label for=ssid>Network name</label><input id=ssid name=ssid maxlength=32 required "
                                  "autocapitalize=off autocorrect=off>"
                                  "<label for=pass>Password</label><input id=pass name=pass type=password maxlength=64>"
                                  "<label class=cb><input type=checkbox name=forget value=1> Forget the other saved networks</label>"
                                  "<button type=submit>Save and restart</button></form>");
    httpd_resp_sendstr_chunk(req, "<h2 style='font-size:1.05em;margin-top:28px'>Saved networks</h2><ul>");
    if (g_config.wifi_count == 0) httpd_resp_sendstr_chunk(req, "<li class=note>None yet</li>");
    for (int i = 0; i < g_config.wifi_count; i++) {
        html_escape(g_config.wifi[i].ssid, esc, sizeof esc);
        snprintf(line, sizeof line, "<li>%s%s</li>", esc, i == 0 ? " (tried first)" : "");
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "</ul><p class=note>Up to 3 networks are kept; the recorder tries them in order.</p></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* x-www-form-urlencoded value of `key`, decoded. */
static bool form_value(const char *body, const char *key, char *out, size_t len) {
    const size_t klen = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p != '\0' && *p != '&' && o + 1 < len) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    const char hex[3] = {p[1], p[2], 0};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) p++;
    }
    out[0] = '\0';
    return false;
}

static void restart_later(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t post_save(httpd_req_t *req) {
    char body[MAX_BODY + 1];
    if (req->content_len <= 0 || req->content_len > MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';
    char ssid[33], pass[65], forget[4];
    form_value(body, "ssid", ssid, sizeof ssid);
    form_value(body, "pass", pass, sizeof pass);
    const bool forget_others = form_value(body, "forget", forget, sizeof forget);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    if (ssid[0] == '\0' || (pass[0] != '\0' && strlen(pass) < 8)) {
        httpd_resp_sendstr_chunk(req, "<h1>Not saved</h1><p>Enter the network name, and a password of at least "
                                      "8 characters (leave it empty only for an open network).</p>"
                                      "<p><a href=/>Back</a></p></body></html>");
        return httpd_resp_sendstr_chunk(req, NULL);
    }
    if (!config_save_wifi(ssid, pass, forget_others)) {
        httpd_resp_sendstr_chunk(req, "<h1>Not saved</h1><p>The recorder could not write its settings. "
                                      "Try again.</p><p><a href=/>Back</a></p></body></html>");
        return httpd_resp_sendstr_chunk(req, NULL);
    }
    char esc[100], line[320];
    html_escape(ssid, esc, sizeof esc);
    snprintf(line, sizeof line, "<h1>Saved</h1><p>The recorder restarts now and joins <b>%s</b>. "
                                "You can close this page.</p></body></html>", esc);
    httpd_resp_sendstr_chunk(req, line);
    httpd_resp_sendstr_chunk(req, NULL);
    ESP_LOGI(TAG, "saved network %s, restarting", ssid);
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Phones probe these to detect a login page; send them to the form. */
static esp_err_t redirect_root(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ── setup mode ─────────────────────────────────────────────────────────── */

void wifi_setup_run(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_name, sizeof s_ap_name, "Recorder-%02X%02X", mac[4], mac[5]);
    /* Per-board password, shown on the display: only someone at the recorder can change its Wi-Fi. */
    snprintf(s_ap_pass, sizeof s_ap_pass, "%02x%02x%02x%02x", mac[2] ^ 0x5a, mac[3] ^ 0xa5, mac[4] ^ 0x3c, mac[5] ^ 0xc3);
    s_active = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_name, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_name);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof ap.ap.password);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Scan once for the list on the page (the phone sees the same networks). */
    const wifi_scan_config_t scan = {.show_hidden = false};
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
        s_scan_count = MAX_SCAN;
        esp_wifi_scan_get_ap_records(&s_scan_count, s_scan);
    }
    ESP_LOGI(TAG, "setup mode: join %s (password %s), open http://192.168.4.1", s_ap_name, s_ap_pass);
    for (uint16_t i = 0; i < s_scan_count; i++) ESP_LOGI(TAG, "  in range: %s (%d dBm)", s_scan[i].ssid, s_scan[i].rssi);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_root},
        {.uri = "/save", .method = HTTP_POST, .handler = post_save},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = redirect_root},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_root},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(server, &routes[i]);

    const int64_t started = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* Don't strand a configured recorder in setup mode if nobody uses the page. */
        if (g_config.wifi_count > 0 && esp_timer_get_time() - started > SETUP_TIMEOUT_US) {
            ESP_LOGW(TAG, "setup timed out, restarting");
            esp_restart();
        }
    }
}
