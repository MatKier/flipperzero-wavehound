idf_component_register(
    SRCS "main.c" "proto.c" "radio.c" "led.c" "info.c" "net_discover.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash driver
                  esp_app_format esp_timer spi_flash esp_hw_support
                  app_update esp_partition lwip
)
