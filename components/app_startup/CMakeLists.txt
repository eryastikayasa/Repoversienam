idf_component_register(
    SRCS "app_startup.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        audio_hal
        audio_engine
        display
        uart_control
        web_config
        wifi_manager
        websocket
        nvs_flash
        esp_netif
        esp_event
)
