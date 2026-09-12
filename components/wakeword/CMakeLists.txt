idf_component_register(
    SRCS "wakeword.cpp"
    INCLUDE_DIRS "include"
    REQUIRES audio_hal esp-sr esp_timer
)
