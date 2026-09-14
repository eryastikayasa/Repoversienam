idf_component_register(
    SRCS
        "websocket.cpp"
        "websocket_transport.cpp"
        "websocket_transport_profile.cpp"
        "websocket_event.cpp"
        "websocket_audio.cpp"
        "gemini/gemini_protocol.cpp"
        "gemini/gemini_message.cpp"
        "gemini/gemini_audio.cpp"
        "gemini/gemini_tool.cpp"
    INCLUDE_DIRS
        "include"
        "gemini"
    REQUIRES
        esp_websocket_client
        esp-tls
        mbedtls
        esp_timer
        audio_hal
        web_config
        cjson
        uart_control
)

# Diagnostic-only linker wrappers. These measure the actual synchronous
# transport/TLS and audio-capture stages without changing ownership, queue
# depth, task priority, or core affinity.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_transport_poll_write"
    "-Wl,--wrap=esp_transport_write"
    "-Wl,--wrap=esp_tls_conn_write"
    "-Wl,--wrap=audio_read_mic"
    "-Wl,--wrap=i2s_channel_read"
    "-Wl,--wrap=aec_process"
    "-Wl,--wrap=xQueueGenericSend"
    "-Wl,--wrap=websocket_transport_send_text"
)
