#include "audio_engine.h"
#include "audio_hal.h"
#include "display_face.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "AUDIO_ENGINE";
static constexpr uint32_t OUTPUT_RATE = 24000U;
static constexpr size_t RING_BYTES = 512U * 1024U;
static constexpr size_t PREBUFFER_BYTES = 128U * 1024U;
static constexpr size_t WARNING_BYTES = 32U * 1024U;
static constexpr size_t CRITICAL_BYTES = 16U * 1024U;
static constexpr size_t PLAYBACK_CHUNK = 2048U;
static constexpr TickType_t LOCK_TIMEOUT = pdMS_TO_TICKS(2);
static constexpr TickType_t PLAYBACK_YIELD = pdMS_TO_TICKS(1);

static volatile bool s_initialized = false;
static volatile audio_engine_state_t s_state = AUDIO_ENGINE_IDLE;
static audio_engine_turn_t s_turn = {};
static TaskHandle_t s_playback_task = nullptr;
static StreamBufferHandle_t s_stream = nullptr;
static SemaphoreHandle_t s_lock = nullptr;
static StaticSemaphore_t s_lock_storage;
static StaticStreamBuffer_t s_stream_storage;
static uint8_t *s_stream_mem = nullptr;
static int64_t s_last_audio_us = 0;
static uint8_t s_level = 0;
static volatile bool s_input_session_after_drain = false;

static const char *state_name(audio_engine_state_t state)
{
    switch (state) {
        case AUDIO_ENGINE_IDLE: return "IDLE";
        case AUDIO_ENGINE_LISTENING: return "LISTENING";
        case AUDIO_ENGINE_THINKING: return "THINKING";
        case AUDIO_ENGINE_BUFFERING: return "BUFFERING";
        case AUDIO_ENGINE_PLAYING: return "PLAYING";
        case AUDIO_ENGINE_PLAYING_LOW: return "PLAYING_LOW";
        case AUDIO_ENGINE_DRAINING: return "DRAINING";
        case AUDIO_ENGINE_COMPLETE: return "COMPLETE";
        case AUDIO_ENGINE_INTERRUPTED: return "INTERRUPTED";
        case AUDIO_ENGINE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void set_state(audio_engine_state_t next)
{
    if (s_state == next) return;
    ESP_LOGI(TAG, "STATE: %s -> %s", state_name(s_state), state_name(next));
    s_state = next;
}

static size_t pending_unlocked(void)
{
    return s_stream ? xStreamBufferBytesAvailable(s_stream) : 0;
}

static size_t pending(void)
{
    if (!s_stream) return 0;
    if (s_lock && xSemaphoreTake(s_lock, LOCK_TIMEOUT) != pdTRUE) return 0;
    const size_t n = pending_unlocked();
    if (s_lock) xSemaphoreGive(s_lock);
    return n;
}

static void flush_stream(void)
{
    if (s_lock && xSemaphoreTake(s_lock, LOCK_TIMEOUT) != pdTRUE) {
        ESP_LOGW(TAG, "Playback flush deferred: mutex busy");
        return;
    }
    if (s_stream) xStreamBufferReset(s_stream);
    if (s_lock) xSemaphoreGive(s_lock);
    s_turn.pending_bytes = 0;
    s_level = 0;
    s_last_audio_us = 0;
}

static void reset_turn(uint32_t generation)
{
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.generation = generation;
    s_level = 0;
    s_last_audio_us = 0;
}

static void update_level(size_t n)
{
    uint8_t level = 0;
    if (n >= PREBUFFER_BYTES) level = 3;
    else if (n >= WARNING_BYTES) level = 2;
    else if (n >= CRITICAL_BYTES) level = 1;
    if (level == s_level) return;
    s_level = level;
    if (level == 1) ESP_LOGW(TAG, "Playback buffer critical: %u B", (unsigned)n);
    else if (level == 2) ESP_LOGW(TAG, "Playback buffer low: %u B", (unsigned)n);
}

void audio_engine_log_diagnostics(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "RAM AUDIT[%s] internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u minimum_free=%u",
             stage ? stage : "unknown", (unsigned)internal_free, (unsigned)internal_largest,
             (unsigned)psram_free, (unsigned)psram_largest, (unsigned)min_free);

    if (s_playback_task) {
        ESP_LOGI(TAG, "TASK AUDIT audio_playback stack=%uB watermark=%uB priority=%u core=%d",
                 4096U, (unsigned)(uxTaskGetStackHighWaterMark(s_playback_task) * sizeof(StackType_t)),
                 (unsigned)uxTaskPriorityGet(s_playback_task), (int)xTaskGetCoreID(s_playback_task));
    }
    ESP_LOGI(TAG, "AUDIO RAM MAP ring=%uB PSRAM/non-realtime prebuffer=%uB output_chunk=%uB",
             (unsigned)RING_BYTES, (unsigned)PREBUFFER_BYTES, (unsigned)PLAYBACK_CHUNK);
}

static void playback_task(void *arg)
{
    (void)arg;
    static uint8_t pcm[PLAYBACK_CHUNK];
    bool started = false;
    int64_t last_stats_us = 0;
    int64_t last_diag_us = 0;

    ESP_LOGI(TAG, "Playback task: PCM16 mono %uHz, ring=%uB, prebuffer=%uB, chunk=%uB",
             (unsigned)OUTPUT_RATE, (unsigned)RING_BYTES,
             (unsigned)PREBUFFER_BYTES, (unsigned)PLAYBACK_CHUNK);

    for (;;) {
        if (!s_stream) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const size_t before = pending();
        update_level(before);
        const bool active = audio_engine_turn_active();

        if (!started && active && !s_turn.model_complete && before < PREBUFFER_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t got = 0;
        if (s_lock && xSemaphoreTake(s_lock, LOCK_TIMEOUT) == pdTRUE) {
            got = xStreamBufferReceive(s_stream, pcm, sizeof(pcm), 0);
            xSemaphoreGive(s_lock);
        } else if (!s_lock) {
            got = xStreamBufferReceive(s_stream, pcm, sizeof(pcm), 0);
        }

        if (got == 0) {
            if (s_turn.model_complete && pending() == 0) {
                const bool start_input_after_drain = s_input_session_after_drain;
                s_input_session_after_drain = false;
                s_turn.playback_drained = true;
                s_turn.pending_bytes = 0;
                started = false;
                s_turn.model_complete = false;
                set_state(AUDIO_ENGINE_COMPLETE);
                display_face_set_state(FACE_LISTENING);
                set_state(AUDIO_ENGINE_IDLE);
                if (start_input_after_drain) {
                    audio_engine_start_input_session();
                    ESP_LOGI(TAG, "Greeting playback drained -> MIC input session started");
                }
            } else if (started && active && !s_turn.model_complete) {
                ++s_turn.underrun_count;
                started = false;
                set_state(AUDIO_ENGINE_BUFFERING);
                ESP_LOGW(TAG, "Playback underrun #%lu", (unsigned long)s_turn.underrun_count);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        got &= ~((size_t)1);
        if (!got) {
            vTaskDelay(PLAYBACK_YIELD);
            continue;
        }

        if (!started) {
            started = true;
            s_turn.playback_started = true;
            set_state(AUDIO_ENGINE_PLAYING);
            display_face_set_state(FACE_SPEAKING);
        }

        const size_t played = audio_write_speaker(pcm, got);
        const bool first_play = (s_turn.bytes_played == 0);
        s_turn.bytes_played += played;
        if (played < got) s_turn.playback_drop += got - played;
        s_turn.pending_bytes = pending();
        s_last_audio_us = esp_timer_get_time();
        update_level(s_turn.pending_bytes);

        if (first_play) {
            if (played > 0) {
                ESP_LOGI(TAG, "AUDIO_ENGINE PCM PLAYED: audio_write_speaker=%uB pending=%uB", (unsigned)played, (unsigned)s_turn.pending_bytes);
            } else {
                ESP_LOGW(TAG, "AUDIO_ENGINE PCM PLAYED: audio_write_speaker returned 0 for %uB", (unsigned)got);
            }
        }

        const int64_t now = esp_timer_get_time();
        if (!last_stats_us || now - last_stats_us >= 1000000LL) {
            last_stats_us = now;
            ESP_LOGI(TAG, "FLOW pending=%u rx=%llu queued=%llu played=%llu drop_net=%llu drop_play=%llu underrun=%lu",
                     (unsigned)s_turn.pending_bytes,
                     (unsigned long long)s_turn.bytes_received,
                     (unsigned long long)s_turn.bytes_queued,
                     (unsigned long long)s_turn.bytes_played,
                     (unsigned long long)s_turn.network_drop,
                     (unsigned long long)s_turn.playback_drop,
                     (unsigned long)s_turn.underrun_count);
        }
        if (!last_diag_us || now - last_diag_us >= 5000000LL) {
            last_diag_us = now;
            audio_engine_log_diagnostics("playback");
        }

        vTaskDelay(PLAYBACK_YIELD);
    }
}

bool audio_engine_init(void)
{
    if (s_initialized) return true;

    s_stream_mem = (uint8_t *)heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stream_mem) s_stream_mem = (uint8_t *)heap_caps_malloc(RING_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stream_mem) {
        ESP_LOGE(TAG, "Audio ring allocation gagal: %uB", (unsigned)RING_BYTES);
        s_state = AUDIO_ENGINE_ERROR;
        return false;
    }

    s_stream = xStreamBufferCreateStatic(RING_BYTES, PLAYBACK_CHUNK, s_stream_mem, &s_stream_storage);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_stream || !s_lock) {
        ESP_LOGE(TAG, "Audio stream/mutex init gagal");
        s_state = AUDIO_ENGINE_ERROR;
        return false;
    }

    reset_turn(0);
    if (xTaskCreatePinnedToCore(playback_task, "audio_playback", 4096, nullptr, 4, &s_playback_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "Playback task create gagal");
        s_state = AUDIO_ENGINE_ERROR;
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "AudioEngine aktif: output=PCM16/24kHz, ring=%uB PSRAM, prebuffer=%uB (~%ums)",
             (unsigned)RING_BYTES, (unsigned)PREBUFFER_BYTES,
             (unsigned)((PREBUFFER_BYTES * 1000U) / (OUTPUT_RATE * 2U)));
    audio_engine_log_diagnostics("init");
    return true;
}

audio_engine_state_t audio_engine_get_state(void) { return s_state; }
const char *audio_engine_state_name(audio_engine_state_t state) { return state_name(state); }

bool audio_engine_turn_active(void)
{
    switch (s_state) {
        case AUDIO_ENGINE_BUFFERING:
        case AUDIO_ENGINE_PLAYING:
        case AUDIO_ENGINE_PLAYING_LOW:
        case AUDIO_ENGINE_DRAINING:
            return true;
        default:
            return false;
    }
}

const audio_engine_turn_t *audio_engine_get_turn(void) { return &s_turn; }

void audio_engine_notify(audio_engine_event_type_t event, uint32_t generation)
{
    if (!s_initialized) return;

    if (event == AUDIO_ENGINE_EVENT_GENERATION_CHANGED) {
        flush_stream();
        reset_turn(generation);
        s_input_session_after_drain = false;
        set_state(AUDIO_ENGINE_IDLE);
        return;
    }

    if (generation && s_turn.generation && generation != s_turn.generation) return;
    if (generation && !s_turn.generation) s_turn.generation = generation;

    switch (event) {
        case AUDIO_ENGINE_EVENT_MODEL_BEGIN:
            flush_stream();
            reset_turn(generation);
            s_input_session_after_drain = false;
            set_state(AUDIO_ENGINE_BUFFERING);
            audio_engine_log_diagnostics("model_begin");
            break;
        case AUDIO_ENGINE_EVENT_MODEL_AUDIO:
            s_turn.model_started = true;
            if (s_state == AUDIO_ENGINE_IDLE || s_state == AUDIO_ENGINE_INTERRUPTED || s_state == AUDIO_ENGINE_COMPLETE)
                set_state(AUDIO_ENGINE_BUFFERING);
            break;
        case AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE:
            s_turn.model_complete = true;
            set_state(AUDIO_ENGINE_DRAINING);
            break;
        case AUDIO_ENGINE_EVENT_INTERRUPT:
            flush_stream();
            s_turn.playback_drained = true;
            s_turn.model_complete = false;
            s_input_session_after_drain = false;
            set_state(AUDIO_ENGINE_INTERRUPTED);
            break;
        case AUDIO_ENGINE_EVENT_ERROR:
            set_state(AUDIO_ENGINE_ERROR);
            break;
        default:
            break;
    }
}

bool audio_engine_push_model_audio(const uint8_t *pcm, size_t len, uint32_t generation)
{
    if (!s_initialized || !s_stream || !pcm || !len) return false;
    if (generation && s_turn.generation && generation != s_turn.generation) return false;
    len &= ~((size_t)1);
    if (!len) return false;

    if (s_turn.model_complete || s_state == AUDIO_ENGINE_INTERRUPTED || s_state == AUDIO_ENGINE_COMPLETE) {
        flush_stream();
        reset_turn(generation ? generation : s_turn.generation);
        set_state(AUDIO_ENGINE_BUFFERING);
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t chunk = (len - offset > PLAYBACK_CHUNK) ? PLAYBACK_CHUNK : (len - offset);
        size_t written = 0;
        if (s_lock && xSemaphoreTake(s_lock, LOCK_TIMEOUT) == pdTRUE) {
            written = xStreamBufferSend(s_stream, pcm + offset, chunk, 0);
            xSemaphoreGive(s_lock);
        } else if (!s_lock) {
            written = xStreamBufferSend(s_stream, pcm + offset, chunk, 0);
        }
        if (!written) {
            s_turn.network_drop += len - offset;
            return offset != 0;
        }
        s_turn.bytes_received += written;
        s_turn.bytes_queued += written;
        offset += written;
        if (written < chunk) {
            s_turn.network_drop += chunk - written;
            return true;
        }
    }
    return true;
}

void audio_engine_request_input_session_after_drain(void)
{
    if (!s_initialized) return;
    s_input_session_after_drain = true;
    ESP_LOGI(TAG, "MIC input session requested after playback drain");
}
