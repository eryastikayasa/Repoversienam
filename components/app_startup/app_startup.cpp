#include "app_startup.h"
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

extern "C" void app_startup_run(void)
{
    ESP_LOGI(TAG, "Repo6 startup: app_startup_run()");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
        ESP_LOGW(TAG, "Konfigurasi belum lengkap -> Web Config");
        web_config_start();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    audio_hal_init();
    audio_hal_ns_init();
    if (!audio_engine_init()) return;

    wifi_init_sta();
    if (!wifi_wait_for_connection(30000)) return;
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (!wakeword_init()) return;
    if (!wakeword_start(on_wakeword_detected, nullptr)) return;
    if (!audio_engine_start_capture()) return;

    ESP_LOGI(TAG, "SISTEM SIAP: Web Config -> NVS -> WiFi -> Wake Word");
    ESP_LOGI(TAG, "Wake word aktif: HI, ESP");

    for (;;) {
        if (s_assistant_requested && !websocket_is_connected()) {
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Wake Word -> buka sesi Gemini");
            websocket_init();
            if (!websocket_connect()) {
                ESP_LOGW(TAG, "WebSocket Gemini gagal -> Wake Word");
                (void)wakeword_rearm();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        if (!websocket_is_connected() && websocket_take_resume_request()) {
            ESP_LOGW(TAG, "Gemini GoAway: mencoba session resumption (timeLeft=%llums)",
                     (unsigned long long)websocket_goaway_time_left_ms());
            if (!websocket_connect()) {
                ESP_LOGW(TAG, "Session resumption gagal -> Wake Word");
                s_session_was_connected = false;
                s_assistant_requested = false;
                (void)wakeword_rearm();
            }
        }

        const bool connected = websocket_is_connected();
        if (connected && !s_session_was_connected) {
            s_session_was_connected = true;
            ESP_LOGI(TAG, "PIPELINE READY:");
            ESP_LOGI(TAG, "MIC -> Audio HAL -> Audio Engine -> WebSocket -> Gemini");
            ESP_LOGI(TAG, "Gemini -> WebSocket -> Audio Engine -> Audio HAL -> SPEAKER");
        } else if (!connected && s_session_was_connected) {
            s_session_was_connected = false;
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Sesi Gemini selesai -> kembali menunggu Wake Word");
            (void)wakeword_rearm();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
