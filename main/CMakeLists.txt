idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos driver esp_timer spi_flash esp_wifi nvs_flash)