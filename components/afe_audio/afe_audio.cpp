#include "afe_audio.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "model_path.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "AFE_AUDIO";

namespace {
static srmodel_list_t *s_models = nullptr;
static const esp_afe_sr_iface_t *s_handle = nullptr;
static esp_afe_sr_data_t *s_data = nullptr;
static int s_feed_samples = 0;
static int s_fetch_samples = 0;
static int s_sample_rate = 0;

/*
 * WakeWord/ESP-SR style feed/fetch decoupling:
 * - caller feeds exact AFE chunks reported by get_feed_chunksize()
 * - dedicated fetch task waits for AFE processing results
 * - processed PCM is queued back to the Gemini audio path
 *
 * This avoids blocking the MIC reader while waiting for the AFE's
 * asynchronous processing pipeline.
 */
static QueueHandle_t s_output_queue = nullptr;
static TaskHandle_t s_fetch_task = nullptr;
static volatile bool s_fetch_running = false;

static constexpr size_t AFE_OUTPUT_QUEUE_DEPTH = 8;
static constexpr TickType_t AFE_FETCH_WAIT_TICKS = pdMS_TO_TICKS(100);

static void reset_state()
{
    s_models = nullptr;
    s_handle = nullptr;
    s_data = nullptr;
    s_feed_samples = 0;
    s_fetch_samples = 0;
    s_sample_rate = 0;
    s_output_queue = nullptr;
    s_fetch_task = nullptr;
    s_fetch_running = false;
}

static void afe_fetch_worker(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "AFE fetch task started");

    while (s_fetch_running && s_handle && s_data) {
        /*
         * ESP-SR AFE processing is asynchronous. Do not poll with a 1 ms
         * timeout from the MIC task. Wait for the result independently.
         */
        afe_fetch_result_t *result =
            s_handle->fetch_with_delay(s_data, AFE_FETCH_WAIT_TICKS);

        if (!s_fetch_running) break;

        if (!result) continue;

        if (result->ret_value != ESP_OK || !result->data ||
            result->data_size <= 0) {
            continue;
        }

        const size_t samples =
            (size_t)result->data_size / sizeof(int16_t);

        if (samples == 0 || samples > (size_t)s_fetch_samples) {
            ESP_LOGW(TAG, "AFE fetch invalid data_size=%d samples=%u",
                     result->data_size, (unsigned)samples);
            continue;
        }

        /*
         * Queue copies the PCM, so the AFE result buffer remains owned by
         * ESP-SR and is never referenced after this iteration.
         */
        int16_t frame[512] = {};
        if (samples > sizeof(frame) / sizeof(frame[0])) continue;
        memcpy(frame, result->data, samples * sizeof(int16_t));

        if (xQueueSend(s_output_queue, frame, 0) != pdTRUE) {
            /*
             * Output must never block the MIC/feed side. If Gemini is
             * temporarily slower, discard the oldest processed frame and
             * keep the newest one.
             */
            int16_t stale[512] = {};
            (void)xQueueReceive(s_output_queue, stale, 0);
            (void)xQueueSend(s_output_queue, frame, 0);
        }
    }

    s_fetch_task = nullptr;
    vTaskDelete(nullptr);
}

static bool start_fetch_task()
{
    s_output_queue = xQueueCreate(AFE_OUTPUT_QUEUE_DEPTH,
                                  (UBaseType_t)(s_fetch_samples * sizeof(int16_t)));
    if (!s_output_queue) {
        ESP_LOGE(TAG, "AFE output queue allocation failed");
        return false;
    }

    s_fetch_running = true;

    BaseType_t rc = xTaskCreatePinnedToCore(
        afe_fetch_worker,
        "afe_fetch",
        4096,
        nullptr,
        4,
        &s_fetch_task,
        1);

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "AFE fetch task create failed");
        s_fetch_running = false;
        vQueueDelete(s_output_queue);
        s_output_queue = nullptr;
        return false;
    }

    return true;
}
}

extern "C" bool afe_audio_init(void)
{
    if (s_data && s_handle) return true;

    ESP_LOGI(TAG, "Initializing ESP-SR AFE for Gemini path only");

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "esp_srmodel_init(model) failed");
        reset_state();
        return false;
    }

    /*
     * One microphone channel, no playback reference. WakeNet is NOT
     * enabled here; the existing WakeNet path remains untouched.
     */
    afe_config_t *config =
        afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!config) {
        ESP_LOGE(TAG, "afe_config_init(M) failed");
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    config->aec_init = false;
    config->se_init = false;
    config->ns_init = true;
    config->ns_model_name = nullptr;
    config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    config->vad_init = false;
    config->wakenet_init = false;
    config->agc_init = false;
    config->fixed_output_channel = true;
    config->afe_perferred_core = 1;
    config->afe_perferred_priority = 3;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_linear_gain = 1.0f;

    config = afe_config_check(config);
    if (!config) {
        ESP_LOGE(TAG, "afe_config_check failed");
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    afe_config_print(config);

    s_handle = esp_afe_handle_from_config(config);
    if (!s_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(config);
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    s_data = s_handle->create_from_config(config);
    afe_config_free(config);

    if (!s_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    s_feed_samples = s_handle->get_feed_chunksize(s_data);
    s_fetch_samples = s_handle->get_fetch_chunksize(s_data);
    s_sample_rate = s_handle->get_samp_rate(s_data);

    if (s_feed_samples <= 0 || s_fetch_samples <= 0 || s_fetch_samples > 512 || s_sample_rate != 16000) {
        ESP_LOGE(TAG, "Unexpected AFE format: feed=%d fetch=%d rate=%d",
                 s_feed_samples, s_fetch_samples, s_sample_rate);
        s_handle->destroy(s_data);
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    if (!start_fetch_task()) {
        s_handle->destroy(s_data);
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    ESP_LOGI(TAG,
             "AFE READY: NS=WebRTC, VAD=OFF, WakeNet=OFF, feed=%d fetch=%d rate=%d fetch_wait=100ms",
             s_feed_samples, s_fetch_samples, s_sample_rate);
    return true;
}

extern "C" void afe_audio_deinit(void)
{
    s_fetch_running = false;

    if (s_fetch_task) {
        for (int i = 0; i < 100 && s_fetch_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (s_output_queue) {
        vQueueDelete(s_output_queue);
        s_output_queue = nullptr;
    }

    if (s_handle && s_data) {
        s_handle->destroy(s_data);
    }
    if (s_models) {
        esp_srmodel_deinit(s_models);
    }

    reset_state();
}

extern "C" bool afe_audio_is_ready(void)
{
    return s_data != nullptr && s_handle != nullptr &&
           s_output_queue != nullptr && s_fetch_task != nullptr;
}

extern "C" bool afe_audio_process(const int16_t *input, size_t input_samples,
                                  int16_t *output, size_t output_capacity_samples,
                                  size_t *output_samples)
{
    if (output_samples) *output_samples = 0;

    if (!afe_audio_is_ready() || !input || !output || !output_samples) {
        return false;
    }

    if (input_samples != (size_t)s_feed_samples ||
        output_capacity_samples < (size_t)s_fetch_samples) {
        return false;
    }

    /*
     * Feed is intentionally non-blocking. The caller supplies exactly the
     * runtime get_feed_chunksize() value; fetch is handled independently by
     * the dedicated ESP-SR task.
     */
    const int fed = s_handle->feed(s_data, input);
    if (fed <= 0) {
        return false;
    }

    /* Output consumption is separate; caller drains all ready frames. */
    (void)output;
    return true;
}

extern "C" bool afe_audio_fetch_output(int16_t *output,
                                       size_t output_capacity_samples,
                                       size_t *output_samples)
{
    if (output_samples) *output_samples = 0;
    if (!afe_audio_is_ready() || !output || !output_samples ||
        output_capacity_samples < (size_t)s_fetch_samples) {
        return false;
    }
    if (xQueueReceive(s_output_queue, output, 0) != pdTRUE) return true;
    *output_samples = (size_t)s_fetch_samples;
    return true;
}

extern "C" int afe_audio_get_feed_samples(void) { return s_feed_samples; }
extern "C" int afe_audio_get_fetch_samples(void) { return s_fetch_samples; }
extern "C" int afe_audio_get_sample_rate(void) { return s_sample_rate; }
