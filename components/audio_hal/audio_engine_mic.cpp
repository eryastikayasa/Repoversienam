#include "audio_engine.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "AUDIO_ENGINE_MIC";

/* Gemini Live target: PCM16, mono, 16 kHz, 20 ms = 640 bytes. */
static constexpr size_t MIC_FRAME_BYTES = 640U;
/* Audio HAL keeps its 512-sample AEC/NS frame, so one read is 32 ms and is
 * then split into 20 ms transport frames without changing the HAL contract. */
static constexpr size_t MIC_READ_BYTES = 1024U;
static constexpr uint32_t MIC_IDLE_TIMEOUT_MS = 60000U;
static constexpr int32_t MIC_ACTIVITY_THRESHOLD = 80;
static constexpr size_t MIC_ACTIVITY_MIN_SAMPLES = 8U;
/* 16 x 20 ms = 320 ms. Bounded and deliberately below a one-second buffer. */
static constexpr size_t MIC_TX_QUEUE_DEPTH = 16U;
static constexpr TickType_t MIC_STOP_WAIT = pdMS_TO_TICKS(1000);

enum mic_owner_t {
    MIC_OWNER_NONE = 0,
    MIC_OWNER_WAKEWORD,
    MIC_OWNER_STOPPING_WAKEWORD,
    MIC_OWNER_GEMINI_WAIT_GREETING_DRAIN,
    MIC_OWNER_GEMINI
};

static audio_engine_mic_frame_cb_t s_mic_listener = nullptr;
static void *s_mic_listener_ctx = nullptr;
static audio_engine_mic_sink_cb_t s_mic_sink = nullptr;
static void *s_mic_sink_ctx = nullptr;
static volatile bool s_capture_started = false;
static volatile bool s_capture_stop_requested = false;
static volatile bool s_input_session_active = false;
static volatile mic_owner_t s_mic_owner = MIC_OWNER_NONE;
static int64_t s_last_activity_us = 0;
static TaskHandle_t s_capture_task = nullptr;
static TaskHandle_t s_sink_task = nullptr;

static StaticQueue_t s_tx_queue_struct;
static uint8_t s_tx_queue_storage[MIC_TX_QUEUE_DEPTH][MIC_FRAME_BYTES];
static QueueHandle_t s_tx_queue = nullptr;
static StaticSemaphore_t s_capture_stopped_storage;
static SemaphoreHandle_t s_capture_stopped = nullptr;
static uint32_t s_tx_queue_drops = 0;

static const char *owner_name(mic_owner_t owner)
{
    switch (owner) {
        case MIC_OWNER_WAKEWORD: return "WAKEWORD";
        case MIC_OWNER_STOPPING_WAKEWORD: return "STOPPING_WAKEWORD";
        case MIC_OWNER_GEMINI_WAIT_GREETING_DRAIN: return "GEMINI_WAIT_GREETING_DRAIN";
        case MIC_OWNER_GEMINI: return "GEMINI";
        default: return "NONE";
    }
}

static void log_owner(mic_owner_t owner)
{
    ESP_LOGI(TAG, "MIC OWNER: %s", owner_name(owner));
}

static bool frame_has_activity(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return false;
    size_t active_samples = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude >= MIC_ACTIVITY_THRESHOLD && ++active_samples >= MIC_ACTIVITY_MIN_SAMPLES)
            return true;
    }
    return false;
}

static void log_mic_task_audit(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "RAM AUDIT[%s] internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage ? stage : "unknown", (unsigned)internal_free, (unsigned)internal_largest,
             (unsigned)psram_free, (unsigned)psram_largest);
    if (s_capture_task) {
        ESP_LOGI(TAG, "TASK AUDIT audio_capture stack=4096B watermark=%uB priority=%u core=%d",
                 (unsigned)(uxTaskGetStackHighWaterMark(s_capture_task) * sizeof(StackType_t)),
                 (unsigned)uxTaskPriorityGet(s_capture_task), (int)xTaskGetCoreID(s_capture_task));
    }
    if (s_sink_task) {
        ESP_LOGI(TAG, "TASK AUDIT mic_tx stack=3072B watermark=%uB priority=%u core=%d",
                 (unsigned)(uxTaskGetStackHighWaterMark(s_sink_task) * sizeof(StackType_t)),
                 (unsigned)uxTaskPriorityGet(s_sink_task), (int)xTaskGetCoreID(s_sink_task));
    }
    ESP_LOGI(TAG, "MIC RAM MAP frame=%uB queue=%u frames=%uB internal/static",
             (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_TX_QUEUE_DEPTH,
             (unsigned)(MIC_TX_QUEUE_DEPTH * MIC_FRAME_BYTES));
}

static void sink_task(void *arg)
{
    (void)arg;
    uint8_t frame[MIC_FRAME_BYTES];
    ESP_LOGI(TAG, "Mic transport worker aktif; queue=%ums, frame=%uB",
             (unsigned)(MIC_TX_QUEUE_DEPTH * 20U), (unsigned)MIC_FRAME_BYTES);
    for (;;) {
        if (xQueueReceive(s_tx_queue, frame, portMAX_DELAY) != pdTRUE) continue;
        if (!s_input_session_active || s_mic_owner != MIC_OWNER_GEMINI) continue;

        audio_engine_mic_sink_cb_t sink = s_mic_sink;
        void *sink_ctx = s_mic_sink_ctx;
        if (sink) sink(frame, MIC_FRAME_BYTES, sink_ctx);
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    static uint8_t read_buffer[MIC_READ_BYTES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    size_t frame_pos = 0;

    ESP_LOGI(TAG, "Mic capture owner aktif: PCM16 16kHz, frame=%uB, read=%uB, idle=%ums owner=%s",
             (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_READ_BYTES, (unsigned)MIC_IDLE_TIMEOUT_MS,
             owner_name(s_mic_owner));

    for (;;) {
        if (s_capture_stop_requested || s_mic_owner == MIC_OWNER_NONE || s_mic_owner == MIC_OWNER_STOPPING_WAKEWORD)
            break;

        const size_t bytes = audio_read_mic(read_buffer, sizeof(read_buffer));
        if (bytes == 0) {
            if (s_capture_stop_requested) break;
            vTaskDelay(1);
            continue;
        }

        if (s_capture_stop_requested || s_mic_owner == MIC_OWNER_NONE || s_mic_owner == MIC_OWNER_STOPPING_WAKEWORD)
            break;

        if (s_mic_owner == MIC_OWNER_WAKEWORD && s_mic_listener)
            s_mic_listener(read_buffer, bytes, s_mic_listener_ctx);

        if (s_capture_stop_requested || s_mic_owner != MIC_OWNER_GEMINI)
            continue;

        size_t offset = 0;
        while (offset < bytes) {
            const size_t copy_len = (MIC_FRAME_BYTES - frame_pos < bytes - offset)
                ? (MIC_FRAME_BYTES - frame_pos) : (bytes - offset);
            memcpy(frame_buffer + frame_pos, read_buffer + offset, copy_len);
            frame_pos += copy_len;
            offset += copy_len;

            if (frame_pos != MIC_FRAME_BYTES) continue;

            frame_pos = 0;

            if (!s_input_session_active || s_mic_owner != MIC_OWNER_GEMINI) {
                vTaskDelay(1);
                continue;
            }

            if (frame_has_activity(frame_buffer, MIC_FRAME_BYTES))
                s_last_activity_us = esp_timer_get_time();

            const int64_t now_us = esp_timer_get_time();
            if (s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_IDLE_TIMEOUT_MS * 1000LL) {
                ESP_LOGI(TAG, "Input idle %ums: AudioEngine mengakhiri sesi MIC",
                         (unsigned)MIC_IDLE_TIMEOUT_MS);
                s_input_session_active = false;
                vTaskDelay(1);
                continue;
            }

            if (s_tx_queue && s_mic_sink) {
                if (xQueueSend(s_tx_queue, frame_buffer, 0) != pdTRUE) {
                    ++s_tx_queue_drops;
                    if ((s_tx_queue_drops & 0x1FU) == 1U) {
                        ESP_LOGW(TAG, "MIC transport queue penuh; drop=%u (queue=%ums)",
                                 (unsigned)s_tx_queue_drops,
                                 (unsigned)(MIC_TX_QUEUE_DEPTH * 20U));
                    }
                }
            }
        }
    }

    s_input_session_active = false;
    s_capture_started = false;
    s_capture_stop_requested = false;
    s_mic_owner = MIC_OWNER_NONE;
    s_capture_task = nullptr;
    log_owner(MIC_OWNER_NONE);
    ESP_LOGI(TAG, "MIC capture task benar-benar berhenti");
    if (s_capture_stopped) xSemaphoreGive(s_capture_stopped);
    vTaskDelete(nullptr);
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

static bool start_capture_for_owner(mic_owner_t owner)
{
    if (owner != MIC_OWNER_WAKEWORD && owner != MIC_OWNER_GEMINI) return false;
    if (s_capture_task) return s_mic_owner == owner;

    if (!s_tx_queue) {
        s_tx_queue = xQueueCreateStatic(
            MIC_TX_QUEUE_DEPTH,
            MIC_FRAME_BYTES,
            &s_tx_queue_storage[0][0],
            &s_tx_queue_struct);
        if (!s_tx_queue) {
            ESP_LOGE(TAG, "Gagal membuat MIC transport queue");
            return false;
        }
    }

    if (!s_capture_stopped)
        s_capture_stopped = xSemaphoreCreateBinaryStatic(&s_capture_stopped_storage);
    if (!s_capture_stopped) {
        ESP_LOGE(TAG, "Gagal membuat MIC capture completion semaphore");
        return false;
    }

    if (!s_sink_task) {
        BaseType_t sink_rc = xTaskCreatePinnedToCore(
            sink_task, "mic_tx", 3072, nullptr, 4, &s_sink_task, 0);
        if (sink_rc != pdPASS) {
            s_sink_task = nullptr;
            ESP_LOGE(TAG, "Gagal membuat MIC transport worker");
            return false;
        }
    }

    s_capture_stop_requested = false;
    s_mic_owner = owner;
    BaseType_t rc = xTaskCreatePinnedToCore(
        capture_task, "audio_capture", 4096, nullptr, 5, &s_capture_task, 1);
    if (rc != pdPASS) {
        s_capture_task = nullptr;
        s_mic_owner = MIC_OWNER_NONE;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine capture task");
        return false;
    }

    s_capture_started = true;
    log_owner(owner);
    log_mic_task_audit("capture_start");
    return true;
}

bool audio_engine_start_capture(void)
{
    if (s_capture_task) return s_mic_owner == MIC_OWNER_WAKEWORD;
    return start_capture_for_owner(MIC_OWNER_WAKEWORD);
}

bool audio_engine_request_capture_stop(void)
{
    if (!s_capture_task) return true;
    if (s_mic_owner == MIC_OWNER_WAKEWORD) {
        s_mic_owner = MIC_OWNER_STOPPING_WAKEWORD;
        log_owner(MIC_OWNER_STOPPING_WAKEWORD);
    }
    s_capture_stop_requested = true;
    return true;
}

bool audio_engine_stop_capture_and_wait(void)
{
    if (!s_capture_task) return true;
    if (!s_capture_stopped) return false;
    (void)audio_engine_request_capture_stop();
    if (xSemaphoreTake(s_capture_stopped, MIC_STOP_WAIT) != pdTRUE) {
        ESP_LOGE(TAG, "MIC capture task tidak berhenti dalam %ums", (unsigned)(MIC_STOP_WAIT * portTICK_PERIOD_MS));
        return false;
    }
    if (s_capture_task) {
        ESP_LOGE(TAG, "MIC capture task handle masih aktif setelah stop");
        return false;
    }
    return true;
}

bool audio_engine_prepare_gemini_input(void)
{
    if (s_capture_task || s_mic_owner == MIC_OWNER_WAKEWORD || s_mic_owner == MIC_OWNER_STOPPING_WAKEWORD) {
        ESP_LOGW(TAG, "MIC owner masih aktif; Gemini input belum boleh mengambil MIC");
        return false;
    }
    s_mic_owner = MIC_OWNER_GEMINI_WAIT_GREETING_DRAIN;
    log_owner(MIC_OWNER_GEMINI_WAIT_GREETING_DRAIN);
    return true;
}

void audio_engine_start_input_session(void)
{
    if (s_mic_owner != MIC_OWNER_GEMINI_WAIT_GREETING_DRAIN) {
        ESP_LOGW(TAG, "MIC session START ditolak: owner=%s, expected=GEMINI_WAIT_GREETING_DRAIN", owner_name(s_mic_owner));
        return;
    }
    if (s_capture_task) {
        ESP_LOGE(TAG, "MIC session START ditolak: capture task masih aktif");
        return;
    }
    if (!start_capture_for_owner(MIC_OWNER_GEMINI)) return;

    s_last_activity_us = esp_timer_get_time();
    s_input_session_active = true;
    ESP_LOGI(TAG, "MIC session START: AudioEngine -> transport");
    log_mic_task_audit("session_start");
}

void audio_engine_stop_input_session(void)
{
    if (!s_input_session_active) return;
    s_input_session_active = false;
    ESP_LOGI(TAG, "MIC session STOP: AudioEngine");
    log_mic_task_audit("session_stop");
}

bool audio_engine_input_session_active(void)
{
    return s_input_session_active && s_mic_owner == MIC_OWNER_GEMINI;
}

bool audio_engine_mic_capture_active(void)
{
    return s_capture_task != nullptr;
}
