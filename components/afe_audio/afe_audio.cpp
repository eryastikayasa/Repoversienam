#include "afe_audio.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_log.h"
#include "model_path.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "AFE_AUDIO";

namespace {
static srmodel_list_t *s_models = nullptr;
static const esp_afe_sr_iface_t *s_handle = nullptr;
static esp_afe_sr_data_t *s_data = nullptr;
static int s_feed_samples = 0;
static int s_fetch_samples = 0;
static int s_sample_rate = 0;

static void reset_state()
{
    s_models = nullptr;
    s_handle = nullptr;
    s_data = nullptr;
    s_feed_samples = 0;
    s_fetch_samples = 0;
    s_sample_rate = 0;
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
     * One microphone channel, no playback reference.  WakeNet is NOT
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

    /* VAD is deliberately disabled in phase 1. Gemini Live keeps its
     * Automatic Activity Detection; AFE must not discard speech frames. */
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

    if (s_feed_samples <= 0 || s_fetch_samples <= 0 || s_sample_rate != 16000) {
        ESP_LOGE(TAG, "Unexpected AFE format: feed=%d fetch=%d rate=%d",
                 s_feed_samples, s_fetch_samples, s_sample_rate);
        s_handle->destroy(s_data);
        esp_srmodel_deinit(s_models);
        reset_state();
        return false;
    }

    ESP_LOGI(TAG,
             "AFE READY: NS=WebRTC, VAD=OFF, WakeNet=OFF, feed=%d fetch=%d rate=%d",
             s_feed_samples, s_fetch_samples, s_sample_rate);
    return true;
}

extern "C" void afe_audio_deinit(void)
{
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
    return s_data != nullptr && s_handle != nullptr;
}

extern "C" bool afe_audio_process(const int16_t *input, size_t input_samples,
                                  int16_t *output, size_t output_capacity_samples,
                                  size_t *output_samples)
{
    if (output_samples) *output_samples = 0;
    if (!afe_audio_is_ready() || !input || !output || !output_samples) return false;
    if (input_samples != (size_t)s_feed_samples ||
        output_capacity_samples < (size_t)s_fetch_samples) {
        return false;
    }

    /* ESP-SR AFE v2 feed() returns the number of bytes written to its
     * input ring, not an esp_err_t. Do not compare it with ESP_OK. */
    const int fed = s_handle->feed(s_data, input);
    if (fed <= 0) {
        return false;
    }

    afe_fetch_result_t *result =
        s_handle->fetch_with_delay(s_data, 1 / portTICK_PERIOD_MS);
    if (!result || result->ret_value != ESP_OK || !result->data ||
        result->data_size <= 0) {
        return false;
    }

    size_t samples = (size_t)result->data_size / sizeof(int16_t);
    if (samples > output_capacity_samples) samples = output_capacity_samples;
    memcpy(output, result->data, samples * sizeof(int16_t));
    *output_samples = samples;
    return true;
}

extern "C" int afe_audio_get_feed_samples(void) { return s_feed_samples; }
extern "C" int afe_audio_get_fetch_samples(void) { return s_fetch_samples; }
extern "C" int afe_audio_get_sample_rate(void) { return s_sample_rate; }
