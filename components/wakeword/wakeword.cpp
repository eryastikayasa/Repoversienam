#include "wakeword.h"
#include "audio_engine.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "WAKEWORD";
static constexpr char WAKE_MODEL_NAME[] = "wn9_hiesp";
static constexpr size_t WAKE_BUFFER_SAMPLES = 1024;

static srmodel_list_t *s_models = nullptr;
static const esp_wn_iface_t *s_iface = nullptr;
static model_iface_data_t *s_model = nullptr;
static int s_chunk_samples = 0;
static bool s_ready = false;
static volatile bool s_armed = false;
static wakeword_detected_cb_t s_callback = nullptr;
static void *s_callback_ctx = nullptr;

static void wakeword_audio_cb(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_ready || !s_armed || !s_iface || !s_model || s_chunk_samples <= 0 || !pcm || len < 2)
        return;
    static int16_t buffer[WAKE_BUFFER_SAMPLES];
    static size_t samples = 0;
    const int16_t *src = reinterpret_cast<const int16_t *>(pcm);
    size_t remaining = len / sizeof(int16_t);
    while (remaining > 0 && s_armed) {
        const size_t capacity = WAKE_BUFFER_SAMPLES - samples;
        if (capacity == 0) { samples = 0; continue; }
        const size_t copy_samples = remaining < capacity ? remaining : capacity;
        memcpy(buffer + samples, src, copy_samples * sizeof(int16_t));
        samples += copy_samples;
        src += copy_samples;
        remaining -= copy_samples;
        while (samples >= static_cast<size_t>(s_chunk_samples) && s_armed) {
            const int result = s_iface->detect(s_model, buffer);
            if (result > 0) {
                s_armed = false;
                samples = 0;
                ESP_LOGW(TAG, "WAKE WORD TERDETEKSI: HI, ESP (id=%d)", result);
                (void)audio_engine_request_capture_stop();
                if (s_callback) s_callback(s_callback_ctx);
                return;
            }
            const size_t remainder = samples - static_cast<size_t>(s_chunk_samples);
            if (remainder > 0) memmove(buffer, buffer + s_chunk_samples, remainder * sizeof(int16_t));
            samples = remainder;
        }
    }
}

bool wakeword_init(void)
{
    if (s_ready) return true;
    ESP_LOGI(TAG, "ESP-SR WakeNet init: %s", WAKE_MODEL_NAME);
    s_models = esp_srmodel_init("model");
    if (!s_models) { ESP_LOGE(TAG, "ESP-SR model loader gagal"); return false; }
    if (esp_srmodel_exists(s_models, (char *)WAKE_MODEL_NAME) < 0) {
        ESP_LOGE(TAG, "WakeNet model tidak ditemukan: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(s_models); s_models = nullptr; return false;
    }
    s_iface = esp_wn_handle_from_name(WAKE_MODEL_NAME);
    if (!s_iface) {
        ESP_LOGE(TAG, "WakeNet handle tidak ditemukan: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(s_models); s_models = nullptr; return false;
    }
    ESP_LOGI(TAG, "Wake heap: PSRAM free=%u internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_model = s_iface->create(WAKE_MODEL_NAME, DET_MODE_90);
    if (!s_model) {
        ESP_LOGE(TAG, "Gagal membuat model WakeNet");
        s_iface = nullptr; esp_srmodel_deinit(s_models); s_models = nullptr; return false;
    }
    s_chunk_samples = s_iface->get_samp_chunksize(s_model);
    const int rate = s_iface->get_samp_rate(s_model);
    const int channels = s_iface->get_channel_num(s_model);
    ESP_LOGI(TAG, "WakeNet ready: rate=%dHz chunk=%d channels=%d", rate, s_chunk_samples, channels);
    if (rate != MIC_SAMPLE_RATE || channels != 1 || s_chunk_samples <= 0 ||
        s_chunk_samples > static_cast<int>(WAKE_BUFFER_SAMPLES)) {
        ESP_LOGE(TAG, "WakeNet audio mismatch: expected %dHz mono", MIC_SAMPLE_RATE);
        s_iface->destroy(s_model); s_model = nullptr; s_iface = nullptr; s_chunk_samples = 0;
        esp_srmodel_deinit(s_models); s_models = nullptr; return false;
    }
    s_ready = true; s_armed = false;
    ESP_LOGI(TAG, "WakeNet siap: HI, ESP");
    return true;
}

bool wakeword_start(wakeword_detected_cb_t cb, void *ctx)
{
    if (!s_ready) return false;
    s_callback = cb; s_callback_ctx = ctx;
    return wakeword_rearm();
}

bool wakeword_stop(void)
{
    if (!s_ready) return false;
    s_armed = false;
    ESP_LOGI(TAG, "Wake word STOP: release MIC capture requested");
    (void)audio_engine_request_capture_stop();
    return true;
}

bool wakeword_rearm(void)
{
    if (!s_ready) return false;
    if (audio_engine_mic_capture_active()) {
        ESP_LOGW(TAG, "WakeWord rearm ditolak: MIC capture masih aktif");
        return false;
    }
    s_armed = true;
    if (!audio_engine_set_mic_listener(wakeword_audio_cb, nullptr)) {
        s_armed = false; return false;
    }
    if (!audio_engine_start_capture()) {
        s_armed = false;
        ESP_LOGE(TAG, "Gagal mengambil kembali MIC untuk WakeWord");
        return false;
    }
    ESP_LOGI(TAG, "Wake word ARMED: HI, ESP");
    return true;
}

bool wakeword_is_ready(void) { return s_ready; }
bool wakeword_is_armed(void) { return s_ready && s_armed; }
