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

/* Gemini Live output: PCM16 mono at 24 kHz. */
static constexpr uint32_t ENGINE_OUTPUT_SAMPLE_RATE = 24000U;
static constexpr uint32_t ENGINE_OUTPUT_BYTES_PER_SEC = ENGINE_OUTPUT_SAMPLE_RATE * 2U;
static constexpr size_t ENGINE_RING_BUFFER_SIZE = 512U * 1024U;
static constexpr size_t ENGINE_PREBUFFER_BYTES = 128U * 1024U;
static constexpr size_t ENGINE_WARNING_BYTES = 64U * 1024U;
static constexpr size_t ENGINE_CRITICAL_BYTES = 32U * 1024U;
static constexpr size_t ENGINE_PLAYBACK_READ_SIZE = 1024U;
static constexpr size_t ENGINE_SEND_CHUNK_SIZE = 1024U;
static constexpr uint32_t ENGINE_I2S_DRAIN_MS = 20U;

/*
 * StreamBuffer remains a non-blocking SPSC pipe:
 *   writer = websocket RX worker
 *   reader = AudioEngine playback task
 *
 * Reset is a third operation that must not race with Send/Receive. The
 * stream mutex serializes only the short, zero-timeout StreamBuffer calls.
 * No audio task ever waits for buffer space while holding this mutex.
 */

static volatile bool s_initialized = false;
static volatile audio_engine_state_t s_state = AUDIO_ENGINE_IDLE;
static audio_engine_turn_t s_turn = {};
static TaskHandle_t s_task = nullptr;
static StreamBufferHandle_t s_stream = nullptr;
static SemaphoreHandle_t s_stream_lock = nullptr;
static StaticSemaphore_t s_stream_lock_storage;
static int64_t s_drain_deadline_us = 0;
static int64_t s_last_queue_us = 0;
static int64_t s_low_since_us = 0;
static size_t s_last_queue_len = 0;
static uint8_t s_buffer_level = 0;
static uint8_t *s_buffer_mem = nullptr;
static StaticStreamBuffer_t s_stream_storage;

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

static void reset_turn(uint32_t generation)
{
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.generation = generation;
}

static size_t pending_bytes_unlocked(void)
{
    return s_stream ? xStreamBufferBytesAvailable(s_stream) : 0;
}

static size_t pending_bytes(void)
{
    if (!s_stream) return 0;
    if (s_stream_lock && xSemaphoreTake(s_stream_lock, portMAX_DELAY) != pdTRUE)
        return 0;
    const size_t pending = pending_bytes_unlocked();
    if (s_stream_lock) xSemaphoreGive(s_stream_lock);
    return pending;
}

static void reset_playback_timeline(void)
{
    s_drain_deadline_us = 0;
    s_last_queue_us = 0;
    s_low_since_us = 0;
    s_last_queue_len = 0;
    s_buffer_level = 0;
}

static void reset_buffer_internal(void)
{
    if (s_stream_lock) xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    if (s_stream) xStreamBufferReset(s_stream);
    if (s_stream_lock) xSemaphoreGive(s_stream_lock);
    reset_playback_timeline();
}

static void update_buffer_level(size_t pending, bool turn_active)
{
    if (!turn_active) {
        s_low_since_us = 0;
        s_buffer_level = 0;
        return;
    }

    if (pending < ENGINE_PREBUFFER_BYTES) {
        if (s_low_since_us == 0) s_low_since_us = esp_timer_get_time();
    } else {
        s_low_since_us = 0;
    }

    uint8_t level = 0;
    if (pending >= ENGINE_RING_BUFFER_SIZE) level = 5;
    else if (pending >= ENGINE_PREBUFFER_BYTES) level = 4;
    else if (pending >= ENGINE_WARNING_BYTES) level = 3;
    else if (pending >= ENGINE_CRITICAL_BYTES) level = 2;
    else if (pending > 0) level = 1;

    if (level == s_buffer_level) return;
    s_buffer_level = level;

    if (level == 1) {
        ESP_LOGW(TAG, "BUFFER CRITICAL: pending=%u B (~%u ms)",
                 (unsigned)pending,
                 (unsigned)((pending * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC));
        if (s_state == AUDIO_ENGINE_PLAYING)
            set_state(AUDIO_ENGINE_PLAYING_LOW);
    } else if (level == 2) {
        ESP_LOGW(TAG, "BUFFER LOW: pending=%u B (~%u ms)",
                 (unsigned)pending,
                 (unsigned)((pending * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC));
    } else if (level >= 4) {
        ESP_LOGI(TAG, "BUFFER TARGET/HIGH: pending=%u B (~%u ms)",
                 (unsigned)pending,
                 (unsigned)((pending * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC));
    }
}

static bool ensure_buffer(void)
{
    if (s_stream) return true;

    s_buffer_mem = (uint8_t *)heap_caps_malloc(
        ENGINE_RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buffer_mem)
        s_buffer_mem = (uint8_t *)heap_caps_malloc(
            ENGINE_RING_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buffer_mem) {
        ESP_LOGE(TAG, "Gagal alokasi audio ring %u byte", (unsigned)ENGINE_RING_BUFFER_SIZE);
        return false;
    }

    s_stream = xStreamBufferCreateStatic(
        ENGINE_RING_BUFFER_SIZE,
        ENGINE_SEND_CHUNK_SIZE,
        s_buffer_mem,
        &s_stream_storage);
    if (!s_stream) {
        heap_caps_free(s_buffer_mem);
        s_buffer_mem = nullptr;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine stream buffer");
        return false;
    }
    return true;
}

static void finish_playback_if_drained(size_t pending)
{
    if (!s_turn.model_complete || pending != 0) {
        s_drain_deadline_us = 0;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (s_drain_deadline_us == 0) {
        s_drain_deadline_us = now_us + ((int64_t)ENGINE_I2S_DRAIN_MS * 1000LL);
        return;
    }
    if (now_us < s_drain_deadline_us) return;

    s_drain_deadline_us = 0;
    s_turn.playback_drained = true;
    set_state(AUDIO_ENGINE_COMPLETE);

    const int64_t network_balance =
        (int64_t)s_turn.bytes_received -
        (int64_t)(s_turn.bytes_queued + s_turn.network_drop);
    const int64_t playback_balance =
        (int64_t)s_turn.bytes_queued -
        (int64_t)(s_turn.bytes_played + s_turn.playback_drop);

    ESP_LOGI(TAG,
             "AUDIO COMPLETE: rx=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu net_bal=%lld play_bal=%lld underrun=%lu",
             (unsigned long long)s_turn.bytes_received,
             (unsigned long long)s_turn.bytes_queued,
             (unsigned long long)s_turn.bytes_played,
             (unsigned long long)s_turn.network_drop,
             (unsigned long long)s_turn.playback_drop,
             (long long)network_balance,
             (long long)playback_balance,
             (unsigned long)s_turn.underrun_count);

    display_face_set_state(FACE_LISTENING);
    /* Consume the completion event so the idle playback loop cannot complete
     * the same turn repeatedly. A new model turn will set this true again. */
    s_turn.model_complete = false;
    set_state(AUDIO_ENGINE_IDLE);
}

static void playback_task(void *arg)
{
    (void)arg;
    static uint8_t playback_buffer[ENGINE_PLAYBACK_READ_SIZE];
    bool playback_started = false;
    bool underrun_reported = false;
    int64_t last_stats_us = 0;

    ESP_LOGI(TAG,
             "AudioEngine playback: %uHz PCM16 mono, ring=%u, prebuffer=%u, warning=%u, critical=%u, read=%u, core=%d priority=5",
             (unsigned)ENGINE_OUTPUT_SAMPLE_RATE,
             (unsigned)ENGINE_RING_BUFFER_SIZE,
             (unsigned)ENGINE_PREBUFFER_BYTES,
             (unsigned)ENGINE_WARNING_BYTES,
             (unsigned)ENGINE_CRITICAL_BYTES,
             (unsigned)ENGINE_PLAYBACK_READ_SIZE,
             xPortGetCoreID());

    for (;;) {
        if (!s_stream) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const size_t pending = pending_bytes();
        const bool active = audio_engine_turn_active();
        update_buffer_level(pending, active);

        if (!playback_started && pending < ENGINE_PREBUFFER_BYTES && active && !s_turn.model_complete) {
            vTaskDelay(1);
            continue;
        }

        if (playback_started && pending == 0 && active && !s_turn.model_complete) {
            if (!underrun_reported) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t input_gap_ms = s_last_queue_us > 0 ?
                    (now_us - s_last_queue_us) / 1000LL : -1LL;
                const int64_t low_for_ms = s_low_since_us > 0 ?
                    (now_us - s_low_since_us) / 1000LL : 0LL;
                ESP_LOGW(TAG,
                         "AUDIO UNDERRUN: input_gap=%lldms low_for=%lldms last_queue=%u rx=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu",
                         (long long)input_gap_ms,
                         (long long)low_for_ms,
                         (unsigned)s_last_queue_len,
                         (unsigned long long)s_turn.bytes_received,
                         (unsigned long long)s_turn.bytes_queued,
                         (unsigned long long)s_turn.bytes_played,
                         (unsigned long long)s_turn.network_drop,
                         (unsigned long long)s_turn.playback_drop);
                ++s_turn.underrun_count;
                underrun_reported = true;
            }
            playback_started = false;
            s_buffer_level = 0;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t received = 0;
        if (s_stream_lock && xSemaphoreTake(s_stream_lock, portMAX_DELAY) == pdTRUE) {
            received = xStreamBufferReceive(
                s_stream, playback_buffer, sizeof(playback_buffer), 0);
            xSemaphoreGive(s_stream_lock);
        } else if (!s_stream_lock) {
            received = xStreamBufferReceive(
                s_stream, playback_buffer, sizeof(playback_buffer), 0);
        }

        if (received == 0) {
            finish_playback_if_drained(pending_bytes());
            vTaskDelay(1);
            continue;
        }

        received &= ~((size_t)1);
        if (received == 0) continue;

        if (!playback_started) {
            playback_started = true;
            underrun_reported = false;
            s_turn.playback_started = true;
            if (s_state == AUDIO_ENGINE_BUFFERING || s_state == AUDIO_ENGINE_PLAYING_LOW)
                set_state(AUDIO_ENGINE_PLAYING);
            display_face_set_state(FACE_SPEAKING);
        }

        const size_t played = audio_write_speaker(playback_buffer, received);
        s_turn.bytes_played += played;

        if (played < received) {
            const size_t dropped = received - played;
            s_turn.playback_drop += dropped;
            ESP_LOGW(TAG, "AUDIO PLAYBACK LOSS: received=%u played=%u dropped=%u",
                     (unsigned)received,
                     (unsigned)played,
                     (unsigned)dropped);
        }

        s_turn.pending_bytes = pending_bytes();
        finish_playback_if_drained(s_turn.pending_bytes);

        const int64_t now_us = esp_timer_get_time();
        if (last_stats_us == 0 || now_us - last_stats_us >= 1000000LL) {
            last_stats_us = now_us;
            ESP_LOGI(TAG,
                     "AUDIO FLOW: pending=%u/%u received=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu",
                     (unsigned)s_turn.pending_bytes,
                     (unsigned)ENGINE_RING_BUFFER_SIZE,
                     (unsigned long long)s_turn.bytes_received,
                     (unsigned long long)s_turn.bytes_queued,
                     (unsigned long long)s_turn.bytes_played,
                     (unsigned long long)s_turn.network_drop,
                     (unsigned long long)s_turn.playback_drop);
        }

        if (!audio_engine_turn_active() && s_turn.pending_bytes == 0) {
            playback_started = false;
            underrun_reported = false;
            reset_playback_timeline();
        }
    }
}

bool audio_engine_init(void)
{
    if (s_initialized) return true;
    if (!ensure_buffer()) {
        s_state = AUDIO_ENGINE_ERROR;
        return false;
    }

    s_stream_lock = xSemaphoreCreateMutexStatic(&s_stream_lock_storage);
    if (!s_stream_lock) {
        s_state = AUDIO_ENGINE_ERROR;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine stream mutex");
        return false;
    }

    reset_turn(0);
    reset_playback_timeline();

    BaseType_t rc = xTaskCreatePinnedToCore(
        playback_task, "audio_playback", 4096, nullptr, 5, &s_task, 0);
    if (rc != pdPASS) {
        s_state = AUDIO_ENGINE_ERROR;
        s_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine playback task");
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "AudioEngine aktif: 1 otak audio | output=%uHz PCM16 | ring=%uB | prebuffer=%uB | stream mutex=short nonblocking ops",
             (unsigned)ENGINE_OUTPUT_SAMPLE_RATE,
             (unsigned)ENGINE_RING_BUFFER_SIZE,
             (unsigned)ENGINE_PREBUFFER_BYTES);
    return true;
}

audio_engine_state_t audio_engine_get_state(void) { return s_state; }
const char *audio_engine_state_name(audio_engine_state_t state) { return state_name(state); }

bool audio_engine_turn_active(void)
{
    const audio_engine_state_t state = s_state;
    return state == AUDIO_ENGINE_BUFFERING ||
           state == AUDIO_ENGINE_PLAYING ||
           state == AUDIO_ENGINE_PLAYING_LOW ||
           state == AUDIO_ENGINE_DRAINING;
}

const audio_engine_turn_t *audio_engine_get_turn(void) { return &s_turn; }

static void begin_turn(uint32_t generation)
{
    if (!s_initialized) return;
    if (generation == 0) generation = s_turn.generation;

    reset_buffer_internal();
    reset_turn(generation);
    set_state(AUDIO_ENGINE_BUFFERING);
}

void audio_engine_notify(audio_engine_event_type_t event, uint32_t generation)
{
    if (!s_initialized) return;

    if (event == AUDIO_ENGINE_EVENT_GENERATION_CHANGED) {
        reset_buffer_internal();
        reset_turn(generation);
        set_state(AUDIO_ENGINE_IDLE);
        return;
    }

    if (generation != 0 && s_turn.generation != 0 && generation != s_turn.generation) {
        reset_buffer_internal();
        reset_turn(generation);
        set_state(AUDIO_ENGINE_IDLE);
    } else if (s_turn.generation == 0 && generation != 0) {
        s_turn.generation = generation;
    }

    switch (event) {
        case AUDIO_ENGINE_EVENT_MODEL_BEGIN:
            if (s_state == AUDIO_ENGINE_COMPLETE || s_state == AUDIO_ENGINE_INTERRUPTED ||
                (s_state == AUDIO_ENGINE_IDLE && s_turn.model_complete)) {
                begin_turn(generation);
            }
            s_turn.model_started = true;
            s_turn.model_complete = false;
            s_turn.playback_started = false;
            s_turn.playback_drained = false;
            set_state(AUDIO_ENGINE_BUFFERING);
            break;

        case AUDIO_ENGINE_EVENT_MODEL_AUDIO:
            s_turn.model_started = true;
            s_turn.model_complete = false;
            if (s_state == AUDIO_ENGINE_IDLE || s_state == AUDIO_ENGINE_LISTENING || s_state == AUDIO_ENGINE_THINKING)
                set_state(AUDIO_ENGINE_BUFFERING);
            break;

        case AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE:
            s_turn.model_complete = true;
            set_state(AUDIO_ENGINE_DRAINING);
            break;

        case AUDIO_ENGINE_EVENT_INTERRUPT:
            reset_buffer_internal();
            set_state(AUDIO_ENGINE_INTERRUPTED);
            reset_turn(generation);
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
    if (!s_initialized || !pcm || len == 0 || !s_stream) return false;
    len &= ~((size_t)1);
    if (len == 0) return false;

    if (s_turn.generation == 0 || s_state == AUDIO_ENGINE_IDLE || s_turn.model_complete) {
        begin_turn(generation);
    } else if (generation != 0 && s_turn.generation != generation) {
        begin_turn(generation);
    }

    s_turn.model_started = true;
    s_turn.model_complete = false;
    if (s_state == AUDIO_ENGINE_IDLE || s_state == AUDIO_ENGINE_LISTENING || s_state == AUDIO_ENGINE_THINKING)
        set_state(AUDIO_ENGINE_BUFFERING);

    const int64_t now_us = esp_timer_get_time();
    if (s_last_queue_us != 0) {
        const int64_t gap_us = now_us - s_last_queue_us;
        if (gap_us >= 100000LL) {
            ESP_LOGW(TAG, "AUDIO INPUT GAP: gap=%lldms len=%u pending=%u",
                     (long long)(gap_us / 1000LL),
                     (unsigned)len,
                     (unsigned)pending_bytes());
        }
    }
    s_last_queue_us = now_us;
    s_last_queue_len = len;
    s_turn.bytes_received += len;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > ENGINE_SEND_CHUNK_SIZE) chunk = ENGINE_SEND_CHUNK_SIZE;
        chunk &= ~((size_t)1);
        if (chunk == 0) break;

        size_t written = 0;
        if (s_stream_lock && xSemaphoreTake(s_stream_lock, portMAX_DELAY) == pdTRUE) {
            written = xStreamBufferSend(s_stream, pcm + offset, chunk, 0);
            xSemaphoreGive(s_stream_lock);
        } else if (!s_stream_lock) {
            written = xStreamBufferSend(s_stream, pcm + offset, chunk, 0);
        }
        if (written == 0) break;

        s_turn.bytes_queued += written;
        offset += written;
    }

    if (offset < len) {
        const size_t dropped = len - offset;
        s_turn.network_drop += dropped;
        ESP_LOGW(TAG, "AUDIO BUFFER DROP: received=%u queued=%u dropped=%u pending=%u",
                 (unsigned)len,
                 (unsigned)offset,
                 (unsigned)dropped,
                 (unsigned)pending_bytes());
    }

    s_turn.pending_bytes = pending_bytes();
    return offset == len;
}
