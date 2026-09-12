idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES audio_hal display uart_control websocket wifi_manager web_config
)
