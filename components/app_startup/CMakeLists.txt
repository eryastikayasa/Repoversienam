idf_component_register(
    SRCS "app_startup.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        audio_hal
        display
        uart_control
        web_config
        wifi_manager
        websocket
        wakeword
        nvs_flash
        esp_netif
        esp_event
)
