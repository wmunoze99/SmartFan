idf_component_register(
    SRCS
        "app_main.cpp"
        "button_controls.cpp"
        "fan_controller.cpp"
        "ota_manager.cpp"
        "remote_logger.cpp"
        "status_display.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update driver espressif__button esp_https_ota esp_lcd esp_netif esp_timer esp_wifi espressif__mdns espressif__qrcode log lwip mbedtls nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
