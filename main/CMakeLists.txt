idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_wifi esp_netif nvs_flash cjson esp_websocket_client
)
