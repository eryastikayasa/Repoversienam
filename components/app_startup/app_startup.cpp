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
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_STARTUP";
static volatile bool s_assistant_requested = false;
static bool s_session_was_connected = false;

static constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
static constexpr TickType_t BOOT_BUTTON_DEBOUNCE = pdMS_TO_TICKS(50);
static int s_boot_raw_level = 1;
static int s_boot_stable_level = 1;
static TickType_t s_boot_last_change = 0;

static void on_wakeword_detected(void *ctx)
{
    (void)ctx;
    s_assistant_requested = true;
}

static void boot_button_init(void)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));

    const int level = gpio_get_level(BOOT_BUTTON_GPIO);
    s_boot_raw_level = level;
    s_boot_stable_level = level;
    s_boot_last_change = xTaskGetTickCount();
    ESP_LOGI(TAG, "BOOT button siap: GPIO0 active-low");
}

static void boot_button_poll(void)
{
    const TickType_t now = xTaskGetTickCount();
    const int raw_level = gpio_get_level(BOOT_BUTTON_GPIO);

    if (raw_level != s_boot_raw_level) {
        s_boot_raw_level = raw_level;
        s_boot_last_change = now;
        return;
    }

    if (raw_level != s_boot_stable_level &&
        (TickType_t)(now - s_boot_last_change) >= BOOT_BUTTON_DEBOUNCE) {
        const int previous_level = s_boot_stable_level;
        s_boot_stable_level = raw_level;

        if (previous_level == 1 && s_boot_stable_level == 0 &&
            !s_assistant_requested && !websocket_is_connected()) {
            ESP_LOGI(TAG, "BOOT Button -> buka sesi Gemini");
            s_assistant_requested = true;
        }
    }
}

static void log_main_task_audit(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TaskHandle_t task = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG,
             "TASK AUDIT main_task stage=%s stack=? watermark=%uB priority=%u core=%d",
             stage ? stage : "unknown",
             (unsigned)(uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t)),
             (unsigned)uxTaskPriorityGet(task),
             (int)xTaskGetCoreID(task));
    ESP_LOGI(TAG,
             "RAM AUDIT[%s] internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage ? stage : "unknown",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest);
}

extern "C" void app_startup_run(void)
{
    ESP_LOGI(TAG, "Repo6 startup: app_startup_run()");
    log_main_task_audit("boot");

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
    audio_engine_log_diagnostics("boot_audio_ready");
    if (!audio_engine_init()) return;

    wifi_init_sta();
    if (!wifi_wait_for_connection(30000)) return;
    esp_wifi_set_ps(WIFI_PS_NONE);
    audio_engine_log_diagnostics("wifi_ready");
    log_main_task_audit("wifi_ready");

    if (!wakeword_init()) return;
    if (!wakeword_start(on_wakeword_detected, nullptr)) return;
    if (!audio_engine_mic_capture_active() && !audio_engine_start_capture()) return;
    boot_button_init();

    ESP_LOGI(TAG, "SISTEM SIAP: Web Config -> NVS -> WiFi -> Wake Word");
    ESP_LOGI(TAG, "Wake word aktif: HI, ESP");
    ESP_LOGI(TAG, "BOOT button aktif: GPIO0");
    audio_engine_log_diagnostics("wakeword_ready");
    log_main_task_audit("wakeword_ready");

    for (;;) {
        boot_button_poll();

        if (s_assistant_requested && !websocket_is_connected()) {
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Trigger -> hentikan WakeWord dan lepaskan MIC");
            (void)wakeword_stop();
            if (!audio_engine_stop_capture_and_wait()) {
                ESP_LOGE(TAG, "MIC ownership transition gagal -> WakeWord");
                (void)wakeword_rearm();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (!audio_engine_prepare_gemini_input()) {
                ESP_LOGE(TAG, "MIC ownership Gemini gagal -> WakeWord");
                (void)wakeword_rearm();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ESP_LOGI(TAG, "Trigger -> buka sesi Gemini");
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
            (void)wakeword_stop();
            if (!audio_engine_stop_capture_and_wait() || !audio_engine_prepare_gemini_input()) {
                ESP_LOGE(TAG, "MIC ownership transition gagal untuk session resumption -> Wake Word");
                s_session_was_connected = false;
                s_assistant_requested = false;
                (void)wakeword_rearm();
                continue;
            }
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
            audio_engine_log_diagnostics("gemini_connected");
        } else if (!connected && s_session_was_connected) {
            s_session_was_connected = false;
            s_assistant_requested = false;
            ESP_LOGI(TAG, "Sesi Gemini selesai -> kembali menunggu Wake Word");
            audio_engine_stop_input_session();
            if (!audio_engine_stop_capture_and_wait()) {
                ESP_LOGE(TAG, "MIC Gemini gagal dilepas saat sesi selesai");
            }
            (void)wakeword_rearm();
            audio_engine_log_diagnostics("gemini_disconnected");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
