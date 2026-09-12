#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "RepoVersiEnam ESP32-S3 architecture initialized");
    ESP_LOGI(TAG, "Baseline: RepoVersiEmpat, prepared for modular services");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
