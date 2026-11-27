# The OLED driver is shared with the recorder; board.h holds the pins for both.
idf_component_register(
    SRCS "bringup_main.c" "../../recorder/main/ssd1306.c"
    INCLUDE_DIRS "." "../../recorder/main" "../../board"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_sdspi fatfs sdmmc esp_wifi esp_netif esp_event
             nvs_flash esp_timer spi_flash)
