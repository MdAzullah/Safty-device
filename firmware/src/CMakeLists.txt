idf_component_register(
    SRCS "main.c" "a9g_module.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_driver_gpio esp_driver_uart esp_timer freertos log esp_common esp_system esp_wifi esp_event nvs_flash esp_http_client esp_netif esp-tls mbedtls
)

