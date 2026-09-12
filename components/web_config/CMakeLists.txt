idf_component_register(
    SRCS "web_config.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi nvs_flash esp_system esp_driver_gpio
)
