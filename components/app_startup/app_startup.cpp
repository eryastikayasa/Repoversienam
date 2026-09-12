#include "audio_hal.h"
#include "audio_engine.h"
#include "display_engine.h"
#include "uart_control.h"
#include "web_config.h"
#include "wifi_manager.h"
#include "websocket.h"
#include "wakeword.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_STARTUP";
static volatile bool s_assistant_requested = false;
static bool s_session_was_connected = false;

static void on_wakeword_detected(void *ctx)
{
    (void)ctx;
    s_assistant_requested = true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Repo6 startup: main.cpp tetap kosong");

    /* 1. NVS is the persistent source for WiFi/API key/Role. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS perlu erase/init ulang");
        if (nvs_flash_erase() == ESP_OK) nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init gagal: %s", esp_err_to_name(nvs_err));
        return;
    }

    /* 2. Web Config runs only when required; values are saved to NVS. */
    display_engine_init();
    display_engine_start();
    uart_control_init();

    if (web_config_is_needed()) {
        ESP_LOGW(TAG, "Konfigurasi belum lengkap -> masuk Web Config");
        web_config_start();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* 3. Hardware + Audio Engine. Wake word listens through Audio Engine. */
    audio_hal_init();
    audio_hal_ns_init();
    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "Audio Engine init gagal");
        return;
    }

    /* 4. WiFi must be ready before the assistant can contact Gemini. */
    wifi_init_sta();
    if (!wifi_wait_for_connection(30000)) {
        ESP_LOGE(TAG, "Wi-Fi belum READY -> startup dihentikan");
        return;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 5. Wake Word is the gate into an assistant/Gemini session. */
    if (!wakeword_init()) {
        ESP_LOGE(TAG, "WakeNet init gagal -> startup dihentikan");
        return;
    }
    if (!wakeword_start(on_wakeword_detected, nullptr)) {
        ESP_LOGE(TAG, "WakeNet listener gagal dipasang");
        return;
    }

    if (!audio_engine_start_capture()) {
        ESP_LOGE(TAG, "Audio Engine capture init gagal");
        return;
    }

    ESP_LOGI(TAG, "SISTEM SIAP: Web Config -> NVS -> WiFi/API Key/Role -> Wake Word");
    ESP_LOGI(TAG, "Wake word aktif: HI, ESP");
    ESP_LOGI(TAG, "Menunggu Wake Word sebelum membuka sesi Gemini...");

    /* 6. Wake Word opens a transport session. WebSocket remains transport-only. */
    for (;;) {
        if (s_assistant_requested) {
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Wake Word diterima -> mulai sesi Gemini");

            websocket_init();
            if (!websocket_connect()) {
                ESP_LOGE(TAG, "WebSocket Gemini gagal start");
                /* Keep the Wake Word gate armed so the user can try again. */
                (void)wakeword_rearm();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ESP_LOGI(TAG, "Menunggu WebSocket CONNECTED...");
        }

        const bool connected = websocket_is_connected();
        if (connected) {
            if (!s_session_was_connected) {
                s_session_was_connected = true;
                ESP_LOGI(TAG, "PIPELINE READY:");
                ESP_LOGI(TAG, "MIC -> Audio HAL -> Audio Engine -> WebSocket -> Gemini");
                ESP_LOGI(TAG, "Gemini -> WebSocket -> Audio Engine -> Audio HAL -> SPEAKER");
            }
        } else if (s_session_was_connected) {
            /* A completed/failed transport session returns control to Wake Word. */
            s_session_was_connected = false;
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Sesi Gemini berakhir -> kembali menunggu Wake Word");
            (void)wakeword_rearm();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
