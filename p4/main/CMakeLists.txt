idf_component_register(SRCS "main.c" "rung2_display.c" "rung3_touch.c"
                            "rung4_storage.c" "rung5_radio.c"
                            "rung6_lora_rx.cpp" "rung7_c6.c"
                            "rung7c_c6_rom.c" "rung8_wifi.c"
                       PRIV_REQUIRES esp_psram spi_flash esp_timer
                                     esp_lcd esp_driver_i2c esp_mm
                                     fatfs sdmmc esp_driver_sdmmc
                                     esp_partition esp_driver_spi
                                     esp_driver_gpio bootloader_support
                                     esp_app_format esp_wifi esp_netif
                                     esp_event)
