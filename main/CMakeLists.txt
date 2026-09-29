idf_component_register(
    SRCS
        "app_main.cpp"
        "fan_controller.cpp"
        "ota_manager.cpp"
        "status_display.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update driver esp_https_ota esp_lcd espressif__qrcode mbedtls nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
