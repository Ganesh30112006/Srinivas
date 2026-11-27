idf_component_register(
    SRCS "main.c" "app.c" "config.c" "session.c" "chunk_store.c" "audio.c" "net.c" "ui.c" "ssd1306.c" "buttons.c" "wifi_setup.c"
    INCLUDE_DIRS "." "../../board"
    REQUIRES lc_core esp_driver_i2s esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_driver_sdspi
             sdmmc fatfs vfs esp_wifi esp_netif esp_event nvs_flash esp_partition esp_timer json mbedtls mqtt
             esp_http_server
    EMBED_FILES "test_clip.adpcm")
