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
)

# Diagnostic-only linker wrappers for the synchronous WebSocket send path.
# They measure the actual transport/TLS calls made by mic_tx without changing
# the WebSocket or Audio Engine architecture.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_transport_poll_write"
    "-Wl,--wrap=esp_transport_write"
    "-Wl,--wrap=esp_tls_conn_write"
)
