#include "audio_engine.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "AUDIO_ENGINE_MIC";

static constexpr size_t MIC_FRAME_BYTES = 320U;
static constexpr size_t MIC_READ_BYTES = 4096U;
static constexpr uint32_t MIC_IDLE_TIMEOUT_MS = 60000U;
static constexpr int32_t MIC_ACTIVITY_THRESHOLD = 80;
static constexpr size_t MIC_ACTIVITY_MIN_SAMPLES = 8U;
static constexpr size_t MIC_TX_QUEUE_DEPTH = 16U;

static audio_engine_mic_frame_cb_t s_mic_listener = nullptr;
static void *s_mic_listener_ctx = nullptr;
static audio_engine_mic_sink_cb_t s_mic_sink = nullptr;
static void *s_mic_sink_ctx = nullptr;
static volatile bool s_capture_started = false;
static volatile bool s_input_session_active = false;
static int64_t s_last_activity_us = 0;
static TaskHandle_t s_capture_task = nullptr;
static TaskHandle_t s_sink_task = nullptr;

static StaticQueue_t s_tx_queue_struct;
static uint8_t s_tx_queue_storage[MIC_TX_QUEUE_DEPTH][MIC_FRAME_BYTES];
static QueueHandle_t s_tx_queue = nullptr;
static uint32_t s_tx_queue_drops = 0;

static bool frame_has_activity(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return false;
    size_t active_samples = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude >= MIC_ACTIVITY_THRESHOLD && ++active_samples >= MIC_ACTIVITY_MIN_SAMPLES) return true;
    }
    return false;
}

static void sink_task(void *arg)
{
    (void)arg;
    uint8_t frame[MIC_FRAME_BYTES];
    ESP_LOGI(TAG, "Mic transport worker aktif; capture/WakeNet tidak mengerjakan TX");
    for (;;) {
        if (xQueueReceive(s_tx_queue, frame, portMAX_DELAY) != pdTRUE) continue;
        audio_engine_mic_sink_cb_t sink = s_mic_sink;
        void *sink_ctx = s_mic_sink_ctx;
        if (sink && s_input_session_active) sink(frame, MIC_FRAME_BYTES, sink_ctx);
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    static uint8_t read_buffer[MIC_READ_BYTES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    size_t frame_pos = 0;
    ESP_LOGI(TAG, "Mic capture owner aktif: PCM16 16kHz, frame=%uB, read=%uB, idle=%ums, stack=8192",
             (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_READ_BYTES, (unsigned)MIC_IDLE_TIMEOUT_MS);
    for (;;) {
        const size_t bytes = audio_read_mic(read_buffer, sizeof(read_buffer));
        if (bytes == 0) { vTaskDelay(1); continue; }
        if (s_mic_listener) s_mic_listener(read_buffer, bytes, s_mic_listener_ctx);
        size_t offset = 0;
        while (offset < bytes) {
            const size_t copy_len = (MIC_FRAME_BYTES - frame_pos < bytes - offset) ? (MIC_FRAME_BYTES - frame_pos) : (bytes - offset);
            memcpy(frame_buffer + frame_pos, read_buffer + offset, copy_len);
            frame_pos += copy_len;
            offset += copy_len;
            if (frame_pos != MIC_FRAME_BYTES) continue;
            frame_pos = 0;
            if (!s_input_session_active) { vTaskDelay(1); continue; }
            if (frame_has_activity(frame_buffer, MIC_FRAME_BYTES)) s_last_activity_us = esp_timer_get_time();
            const int64_t now_us = esp_timer_get_time();
            if (s_last_activity_us != 0 && now_us - s_last_activity_us >= (int64_t)MIC_IDLE_TIMEOUT_MS * 1000LL) {
                ESP_LOGI(TAG, "Input idle %ums: AudioEngine mengakhiri sesi MIC", (unsigned)MIC_IDLE_TIMEOUT_MS);
                s_input_session_active = false;
                vTaskDelay(1);
                continue;
            }
            if (s_tx_queue && s_mic_sink) {
                if (xQueueSend(s_tx_queue, frame_buffer, 0) != pdTRUE) {
                    ++s_tx_queue_drops;
                    if ((s_tx_queue_drops & 0x3FU) == 1U) ESP_LOGW(TAG, "MIC transport queue penuh; frame drop total=%u", (unsigned)s_tx_queue_drops);
                }
            }
            vTaskDelay(1);
        }
    }
}

bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb, void *ctx)
{
    s_mic_listener = cb;
    s_mic_listener_ctx = ctx;
    return true;
}

bool audio_engine_set_mic_sink(audio_engine_mic_sink_cb_t cb, void *ctx)
{
    s_mic_sink = cb;
    s_mic_sink_ctx = ctx;
    return true;
}

bool audio_engine_start_capture(void)
{
    if (s_capture_started) return true;
    s_tx_queue = xQueueCreateStatic(MIC_TX_QUEUE_DEPTH, MIC_FRAME_BYTES, &s_tx_queue_storage[0][0], &s_tx_queue_struct);
    if (!s_tx_queue) { ESP_LOGE(TAG, "Gagal membuat MIC transport queue"); return false; }
    BaseType_t sink_rc = xTaskCreatePinnedToCore(sink_task, "mic_tx", 4096, nullptr, 4, &s_sink_task, 0);
    if (sink_rc != pdPASS) { s_sink_task = nullptr; s_tx_queue = nullptr; ESP_LOGE(TAG, "Gagal membuat MIC transport worker"); return false; }
    BaseType_t rc = xTaskCreatePinnedToCore(capture_task, "audio_capture", 8192, nullptr, 5, &s_capture_task, 1);
    if (rc != pdPASS) { s_capture_task = nullptr; s_sink_task = nullptr; s_tx_queue = nullptr; ESP_LOGE(TAG, "Gagal membuat AudioEngine capture task"); return false; }
    s_capture_started = true;
    return true;
}

void audio_engine_start_input_session(void)
{
    if (!s_capture_started) { ESP_LOGW(TAG, "start_input_session sebelum capture aktif"); return; }
    s_last_activity_us = esp_timer_get_time();
    s_input_session_active = true;
    ESP_LOGI(TAG, "MIC session START: AudioEngine -> transport");
}

void audio_engine_stop_input_session(void)
{
    if (!s_input_session_active) return;
    s_input_session_active = false;
    ESP_LOGI(TAG, "MIC session STOP: AudioEngine");
}

bool audio_engine_input_session_active(void)
{
    return s_input_session_active;
}
