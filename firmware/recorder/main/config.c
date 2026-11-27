#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config";

device_config_t g_config;

static void read_str(nvs_handle_t nvs, bool opened, const char *key, char *out, size_t len, const char *fallback) {
    size_t size = len;
    if (!opened || nvs_get_str(nvs, key, out, &size) != ESP_OK) strlcpy(out, fallback, len);
}

/* A PEM blob of any size (certificates are ~1.2 KB, keys ~1.7 KB). */
static char *read_blob_str(nvs_handle_t nvs, bool opened, const char *key) {
    size_t size = 0;
    if (!opened || nvs_get_str(nvs, key, NULL, &size) != ESP_OK || size < 2) return NULL;
    char *out = malloc(size);
    if (out != NULL && nvs_get_str(nvs, key, out, &size) != ESP_OK) {
        free(out);
        out = NULL;
    }
    return out;
}

static void wifi_keys(int slot, char *ssid_key, char *pass_key) {
    /* Slot 0 keeps the original key names, so older provisioning files still work. */
    if (slot == 0) {
        strcpy(ssid_key, "wifi_ssid");
        strcpy(pass_key, "wifi_pass");
    } else {
        sprintf(ssid_key, "wifi_ssid%d", slot);
        sprintf(pass_key, "wifi_pass%d", slot);
    }
}

bool config_load(device_config_t *out) {
    free(out->iot_cert);
    free(out->iot_key);
    memset(out, 0, sizeof *out);
    nvs_handle_t nvs = 0;
    const bool opened = nvs_open("lc_cfg", NVS_READONLY, &nvs) == ESP_OK;
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char ssid_key[16], pass_key[16];
        wifi_keys(i, ssid_key, pass_key);
        wifi_network_t *n = &out->wifi[out->wifi_count];
        read_str(nvs, opened, ssid_key, n->ssid, sizeof n->ssid, i == 0 ? CONFIG_LC_WIFI_SSID : "");
        read_str(nvs, opened, pass_key, n->pass, sizeof n->pass, i == 0 ? CONFIG_LC_WIFI_PASSWORD : "");
        if (n->ssid[0] != '\0') out->wifi_count++;
    }
    read_str(nvs, opened, "server_url", out->server_url, sizeof out->server_url, CONFIG_LC_SERVER_URL);
    read_str(nvs, opened, "iot_endpoint", out->iot_endpoint, sizeof out->iot_endpoint, "");
    out->iot_cert = read_blob_str(nvs, opened, "iot_cert");
    out->iot_key = read_blob_str(nvs, opened, "iot_key");
    read_str(nvs, opened, "device_id", out->device_id, sizeof out->device_id, CONFIG_LC_DEVICE_ID);
    read_str(nvs, opened, "device_token", out->device_token, sizeof out->device_token, CONFIG_LC_DEVICE_TOKEN);
    if (opened) nvs_close(nvs);

    if (out->iot_endpoint[0] != '\0' && (out->iot_cert == NULL || out->iot_key == NULL)) {
        ESP_LOGE(TAG, "iot_endpoint is set but iot_cert or iot_key is missing");
        out->iot_endpoint[0] = '\0';
    }
    const bool transport = config_uses_iot(out) || out->server_url[0] != '\0';
    const bool ok = transport && strlen(out->device_id) == 36 && strlen(out->device_token) >= 16;
    if (!ok) ESP_LOGE(TAG, "incomplete configuration: provision NVS (see firmware/README.md)");
    ESP_LOGI(TAG, "device %s → %s, %d Wi-Fi network(s) (secrets not logged)", out->device_id,
             config_uses_iot(out) ? out->iot_endpoint : out->server_url, out->wifi_count);
    return ok;
}

bool config_uses_iot(const device_config_t *cfg) {
    return cfg->iot_endpoint[0] != '\0' && cfg->iot_cert != NULL && cfg->iot_key != NULL;
}

bool config_save_wifi(const char *ssid, const char *pass, bool forget_others) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 || strlen(pass) > 64) return false;
    wifi_network_t list[WIFI_MAX_NETWORKS] = {0};
    int count = 0;
    strlcpy(list[0].ssid, ssid, sizeof list[0].ssid);
    strlcpy(list[0].pass, pass, sizeof list[0].pass);
    count = 1;
    if (!forget_others) {
        for (int i = 0; i < g_config.wifi_count && count < WIFI_MAX_NETWORKS; i++) {
            if (strcmp(g_config.wifi[i].ssid, ssid) == 0) continue;
            list[count++] = g_config.wifi[i];
        }
    }
    nvs_handle_t nvs;
    if (nvs_open("lc_cfg", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < WIFI_MAX_NETWORKS && err == ESP_OK; i++) {
        char ssid_key[16], pass_key[16];
        wifi_keys(i, ssid_key, pass_key);
        if (i < count) {
            err = nvs_set_str(nvs, ssid_key, list[i].ssid);
            if (err == ESP_OK) err = nvs_set_str(nvs, pass_key, list[i].pass);
        } else {
            /* An empty SSID (not erase) so the menuconfig fallback can't come back for slot 0. */
            err = nvs_set_str(nvs, ssid_key, "");
            if (err == ESP_OK) err = nvs_set_str(nvs, pass_key, "");
        }
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "saving Wi-Fi failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
