idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        microlink
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        esp_app_format
        esp_driver_gpio
        espressif__cjson
        lwip
)
