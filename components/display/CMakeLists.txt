idf_component_register(
    SRCS
        "display_engine/display_engine.cpp"
        "display_face/display_face.cpp"
        "display_text/display_text.cpp"
        "display_driver/display_driver.cpp"
    INCLUDE_DIRS
        "display_engine"
        "display_face"
        "display_text"
        "display_driver"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd esp_timer esp_wifi
)
