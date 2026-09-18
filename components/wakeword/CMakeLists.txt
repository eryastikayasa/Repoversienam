idf_component_register(
    SRCS "wakeword.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr audio_hal
)
