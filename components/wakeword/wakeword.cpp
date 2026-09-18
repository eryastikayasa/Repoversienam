#include "wakeword.h"

#include "audio_hal.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

namespace {

static const char *TAG = "WAKEWORD";
static constexpr char MODEL_NAME[] = "wn9_hiesp";
static constexpr size_t PCM_READ_SAMPLES = 512;
static constexpr size_t BUFFER_SAMPLES = 1024;
static constexpr uint32_t WAKEWORD_TASK_STACK = 8192;
static constexpr UBaseType_t WAKEWORD_TASK_PRIORITY = 6;

static srmodel_list_t *s_models = nullptr;
static const esp_wn_iface_t *s_iface = nullptr;
static model_iface_data_t *s_model = nullptr;
static int s_chunk_samples = 0;
static int s_sample_rate = 0;
static int s_channels = 0;
static int16_t s_buffer[BUFFER_SAMPLES];
static size_t s_buffer_samples = 0;
static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;
static volatile bool s_detected = false;

static void reset_state()
{
    s_buffer_samples = 0;
}

static void wakeword_task(void *)
{
    int16_t pcm[PCM_READ_SAMPLES];

    ESP_LOGI(TAG, "task started");

    while (s_running) {
        size_t samples_read = 0;
        const esp_err_t err = audio_hal_read_pcm(
            pcm, PCM_READ_SAMPLES, &samples_read);

        if (err != ESP_OK) {
            if (s_running) {
                ESP_LOGE(TAG, "MIC read gagal: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }

        if (samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t offset = 0;
        while (offset < samples_read && s_running) {
            const size_t free_samples = BUFFER_SAMPLES - s_buffer_samples;
            const size_t copy_samples =
                (samples_read - offset < free_samples)
                    ? (samples_read - offset)
                    : free_samples;

            memcpy(s_buffer + s_buffer_samples,
                   pcm + offset,
                   copy_samples * sizeof(int16_t));
            s_buffer_samples += copy_samples;
            offset += copy_samples;

            while (s_running && s_buffer_samples >= static_cast<size_t>(s_chunk_samples)) {
                const int result = s_iface->detect(s_model, s_buffer);

                if (result > 0) {
                    ESP_LOGI(TAG, "detected HI, ESP (id=%d)", result);
                    s_detected = true;
                    s_running = false;
                    (void)audio_hal_stop_capture();
                    reset_state();
                    break;
                }

                const size_t remaining =
                    s_buffer_samples - static_cast<size_t>(s_chunk_samples);
                if (remaining > 0) {
                    memmove(s_buffer,
                            s_buffer + s_chunk_samples,
                            remaining * sizeof(int16_t));
                }
                s_buffer_samples = remaining;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    (void)audio_hal_stop_capture();
    s_task = nullptr;
    ESP_LOGI(TAG, "task stopped");
    vTaskDelete(nullptr);
}

} // namespace

extern "C" bool wakeword_init(void)
{
    if (s_model && s_iface) return true;

    ESP_LOGI(TAG, "init model=%s", MODEL_NAME);

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "ESP-SR model partition 'model' tidak tersedia");
        return false;
    }

    if (esp_srmodel_exists(s_models, (char *)MODEL_NAME) < 0) {
        ESP_LOGE(TAG, "WakeNet model tidak ditemukan: %s", MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_iface = esp_wn_handle_from_name(MODEL_NAME);
    if (!s_iface) {
        ESP_LOGE(TAG, "WakeNet interface tidak ditemukan: %s", MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_model = s_iface->create(MODEL_NAME, DET_MODE_90);
    if (!s_model) {
        ESP_LOGE(TAG, "Gagal membuat WakeNet model: %s", MODEL_NAME);
        s_iface = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_chunk_samples = s_iface->get_samp_chunksize(s_model);
    s_sample_rate = s_iface->get_samp_rate(s_model);
    s_channels = s_iface->get_channel_num(s_model);
    reset_state();

    ESP_LOGI(TAG, "sample_rate=%d channels=%d chunk_samples=%d",
             s_sample_rate, s_channels, s_chunk_samples);

    if (s_sample_rate != 16000 ||
        s_channels != 1 ||
        s_chunk_samples <= 0 ||
        s_chunk_samples > static_cast<int>(BUFFER_SAMPLES)) {
        ESP_LOGE(TAG, "WakeNet audio configuration tidak kompatibel");
        wakeword_deinit();
        return false;
    }

    s_detected = false;
    ESP_LOGI(TAG, "WakeNet ready: HI, ESP / DET_MODE_90");
    return true;
}

extern "C" bool wakeword_start(void)
{
    if (!s_model || !s_iface || s_chunk_samples <= 0) return false;
    if (s_running) return true;
    if (s_task != nullptr) return false;

    s_detected = false;
    reset_state();

    const esp_err_t err = audio_hal_start_capture();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture start gagal: %s", esp_err_to_name(err));
        return false;
    }

    s_running = true;
    const BaseType_t result = xTaskCreate(
        wakeword_task,
        "wakeword_task",
        WAKEWORD_TASK_STACK,
        nullptr,
        WAKEWORD_TASK_PRIORITY,
        &s_task);

    if (result != pdPASS) {
        s_running = false;
        (void)audio_hal_stop_capture();
        s_task = nullptr;
        ESP_LOGE(TAG, "gagal membuat WakeWord task");
        return false;
    }

    ESP_LOGI(TAG, "capture started");
    return true;
}

extern "C" void wakeword_stop(void)
{
    if (!s_running && s_task == nullptr) {
        (void)audio_hal_stop_capture();
        return;
    }

    s_running = false;
    (void)audio_hal_stop_capture();

    for (uint32_t i = 0; i < 100 && s_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    reset_state();
}

extern "C" bool wakeword_is_running(void)
{
    return s_running;
}

extern "C" bool wakeword_detected(void)
{
    return s_detected;
}

extern "C" void wakeword_clear_detected(void)
{
    s_detected = false;
}

extern "C" int wakeword_get_chunk_samples(void)
{
    return s_chunk_samples;
}

extern "C" int wakeword_get_sample_rate(void)
{
    return s_sample_rate;
}

extern "C" void wakeword_deinit(void)
{
    wakeword_stop();

    if (s_model && s_iface) {
        s_iface->destroy(s_model);
    }

    s_model = nullptr;
    s_iface = nullptr;
    s_chunk_samples = 0;
    s_sample_rate = 0;
    s_channels = 0;
    reset_state();

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
    }
}
