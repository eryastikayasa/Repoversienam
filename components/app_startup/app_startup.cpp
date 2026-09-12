#include "audio_hal.h"
#include "audio_engine.h"
#include "display_engine.h"
#include "uart_control.h"
#include "web_config.h"
#include "wifi_manager.h"
#include "websocket.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_STARTUP";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Repo6 startup: main.cpp tetap kosong");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS perlu erase/init ulang");
        if (nvs_flash_erase() == ESP_OK) nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init gagal: %s", esp_err_to_name(nvs_err));
        return;
    }

    display_engine_init();
    display_engine_start();
    uart_control_init();

    if (web_config_is_needed()) {
        ESP_LOGW(TAG, "Konfigurasi belum lengkap -> masuk Web Config");
        web_config_start();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    audio_hal_init();
    audio_hal_ns_init();

    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "Audio Engine init gagal");
        return;
    }
    if (!audio_engine_start_capture()) {
        ESP_LOGE(TAG, "Audio Engine capture init gagal");
        return;
    }

    wifi_init_sta();
    if (!wifi_wait_for_connection(30000)) {
        ESP_LOGE(TAG, "Wi-Fi belum READY -> startup dihentikan");
        return;
    }

    websocket_init();
    if (!websocket_connect()) {
        ESP_LOGE(TAG, "WebSocket Gemini gagal start");
        return;
    }

    ESP_LOGI(TAG, "PIPELINE READY:");
    ESP_LOGI(TAG, "MIC -> Audio HAL -> Audio Engine -> WebSocket -> Gemini");
    ESP_LOGI(TAG, "Gemini -> WebSocket -> Audio Engine -> Audio HAL -> SPEAKER");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
