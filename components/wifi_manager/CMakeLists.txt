idf_component_register(
    SRCS "wifi_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif web_config nvs_flash esp_timer
)
