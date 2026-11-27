/* Device configuration from NVS namespace "lc_cfg" (D10), falling back to menuconfig values. */
#pragma once

#include <stdbool.h>

#define WIFI_MAX_NETWORKS 3

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_network_t;

typedef struct {
    /* Wi-Fi networks, tried in order. Slot 0 is the newest (set from the setup page). */
    wifi_network_t wifi[WIFI_MAX_NETWORKS];
    int wifi_count;
    /* Transport: AWS IoT Core (MQTT) when iot_endpoint is set, otherwise the WebSocket gateway. */
    char server_url[160];
    char iot_endpoint[128];
    char *iot_cert; /* PEM, heap */
    char *iot_key;  /* PEM, heap */
    char device_id[37];
    char device_token[129];
} device_config_t;

/* Loads the configuration; returns false if anything required (except Wi-Fi) is missing. */
bool config_load(device_config_t *out);

/* True when the device talks to AWS IoT Core instead of the WebSocket gateway. */
bool config_uses_iot(const device_config_t *cfg);

/* Puts a network first in the saved list (replacing one with the same SSID); `forget_others`
 * drops the rest. Written to NVS; takes effect after a restart. */
bool config_save_wifi(const char *ssid, const char *pass, bool forget_others);

extern device_config_t g_config;

#define FIRMWARE_VERSION "1.1.0"
