idf_component_register(
    SRCS
        "audio_hal.cpp"
        "audio_engine.cpp"
        "audio_engine_ingest.cpp"
        "audio_engine_mic.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2s esp-sr esp_timer display mbedtls
)
