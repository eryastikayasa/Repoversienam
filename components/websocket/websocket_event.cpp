#include "websocket_event.h"
#include "websocket_transport.h"
#include "websocket_audio.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static constexpr size_t RX_MAX_PAYLOAD = 64U * 1024U;
/* Transport staging is short-lived: callback -> ready queue -> worker copy. */
static constexpr size_t RX_STAGING_COUNT = 2U;
/* Processing buffers live only for the CPU-heavy Gemini processing lifetime. */
static constexpr size_t RX_PROCESS_COUNT = 3U;
static constexpr uint32_t RX_WORKER_STACK = 8192U;
static constexpr UBaseType_t RX_WORKER_PRIORITY = 3U;
static constexpr BaseType_t RX_WORKER_CORE = 0;
static constexpr uint32_t RX_METRICS_PERIOD_MS = 5000U;

struct rx_item_t { char *buffer; size_t len; uint32_t generation; };

static QueueHandle_t s_rx_staging_free_queue = nullptr;
static QueueHandle_t s_rx_ready_queue = nullptr;
static QueueHandle_t s_rx_process_free_queue = nullptr;
static StaticQueue_t s_rx_staging_free_storage;
static StaticQueue_t s_rx_ready_queue_storage;
static StaticQueue_t s_rx_process_free_storage;
static char *s_rx_staging_free_storage[RX_STAGING_COUNT];
static rx_item_t s_rx_ready_storage[RX_STAGING_COUNT];
static char *s_rx_process_free_storage[RX_PROCESS_COUNT];
static char *s_rx_staging_buffers[RX_STAGING_COUNT] = {};
static char *s_rx_process_buffers[RX_PROCESS_COUNT] = {};

/* The websocket callback owns only one active fragmented message at a time.
 * Its staging buffer is released by ws_rx immediately after the processing
 * buffer copy, before JSON/Base64/audio work begins. */
static char *s_rx_assembling_buffer = nullptr;
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;
static uint8_t s_rx_first_opcode = 0;
static uint32_t s_rx_generation = 0;
static bool s_rx_assembling = false;

/* When an offset-0 fragment cannot be accepted, consume the remaining
 * fragments silently instead of creating a cascade of "fragment invalid"
 * logs. A new offset-0 message always starts a new decision. */
static bool s_rx_discarding = false;
static size_t s_rx_discard_expected = 0;
static size_t s_rx_discard_received = 0;
static uint32_t s_rx_discard_generation = 0;

static TaskHandle_t s_rx_worker_task = nullptr;
static bool s_rx_worker_ready = false;

static volatile uint32_t s_rx_chunks = 0;
static volatile uint64_t s_rx_bytes = 0;
static volatile uint32_t s_rx_payload_drops = 0;
static volatile uint32_t s_rx_fragment_errors = 0;
static volatile uint32_t s_rx_processing_count = 0;
static volatile uint32_t s_rx_callback_max_us = 0;
static volatile uint32_t s_rx_processing_max_us = 0;
static volatile uint32_t s_rx_queue_highwater = 0;
static volatile uint32_t s_rx_free_lowwater = RX_STAGING_COUNT;
static volatile uint32_t s_rx_process_free_lowwater = RX_PROCESS_COUNT;
static volatile uint32_t s_rx_last_starvation_log = 0;

static inline void metric_inc(volatile uint32_t *value) { __atomic_add_fetch(value, 1U, __ATOMIC_RELAXED); }
static inline void metric_add_u64(volatile uint64_t *value, uint64_t amount) { __atomic_add_fetch(value, amount, __ATOMIC_RELAXED); }
static void metric_max(volatile uint32_t *value, uint32_t candidate)
{
    uint32_t old = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (old < candidate && !__atomic_compare_exchange_n(value, &old, candidate, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void note_staging_depth(void)
{
    if (!s_rx_staging_free_queue) return;
    const UBaseType_t free_count = uxQueueMessagesWaiting(s_rx_staging_free_queue);
    uint32_t old_low = __atomic_load_n(&s_rx_free_lowwater, __ATOMIC_RELAXED);
    while (old_low > (uint32_t)free_count &&
           !__atomic_compare_exchange_n(&s_rx_free_lowwater, &old_low, (uint32_t)free_count, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void note_ready_highwater(void)
{
    if (!s_rx_ready_queue) return;
    metric_max(&s_rx_queue_highwater, (uint32_t)uxQueueMessagesWaiting(s_rx_ready_queue));
}

static void note_process_free_depth(void)
{
    if (!s_rx_process_free_queue) return;
    const UBaseType_t free_count = uxQueueMessagesWaiting(s_rx_process_free_queue);
    uint32_t old_low = __atomic_load_n(&s_rx_process_free_lowwater, __ATOMIC_RELAXED);
    while (old_low > (uint32_t)free_count &&
           !__atomic_compare_exchange_n(&s_rx_process_free_lowwater, &old_low, (uint32_t)free_count, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void log_rx_metrics(const char *reason)
{
    ESP_LOGI(TAG,
             "RX METRICS[%s] chunks=%lu bytes=%llu queue_high=%lu free_low=%lu process_free_low=%lu drops=%lu fragments=%lu processed=%lu cb_max_us=%lu process_max_us=%lu",
             reason ? reason : "periodic",
             (unsigned long)__atomic_load_n(&s_rx_chunks, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_rx_bytes, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_queue_highwater, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_free_lowwater, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_process_free_lowwater, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_payload_drops, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_fragment_errors, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_processing_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_callback_max_us, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_rx_processing_max_us, __ATOMIC_RELAXED));
}

static bool return_buffer(QueueHandle_t queue, char **buffer, const char *label)
{
    if (!queue || !buffer || !*buffer) return false;
    if (xQueueSend(queue, buffer, 0) != pdPASS) {
        ESP_LOGE(TAG, "WS_RX: %s buffer return gagal", label ? label : "unknown");
        return false;
    }
    return true;
}

static bool ensure_rx_worker(void)
{
    if (s_rx_worker_ready) return true;

    s_rx_staging_free_queue = xQueueCreateStatic(
        RX_STAGING_COUNT, sizeof(char *), reinterpret_cast<uint8_t *>(s_rx_staging_free_storage), &s_rx_staging_free_storage);
    s_rx_ready_queue = xQueueCreateStatic(
        RX_STAGING_COUNT, sizeof(rx_item_t), reinterpret_cast<uint8_t *>(s_rx_ready_storage), &s_rx_ready_queue_storage);
    s_rx_process_free_queue = xQueueCreateStatic(
        RX_PROCESS_COUNT, sizeof(char *), reinterpret_cast<uint8_t *>(s_rx_process_free_storage), &s_rx_process_free_storage);
    if (!s_rx_staging_free_queue || !s_rx_ready_queue || !s_rx_process_free_queue) {
        ESP_LOGE(TAG, "WS_RX: gagal membuat RX queues");
        return false;
    }

    size_t staging_allocated = 0;
    for (; staging_allocated < RX_STAGING_COUNT; ++staging_allocated) {
        s_rx_staging_buffers[staging_allocated] = static_cast<char *>(
            heap_caps_malloc(RX_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rx_staging_buffers[staging_allocated]) break;
        if (xQueueSend(s_rx_staging_free_queue, &s_rx_staging_buffers[staging_allocated], 0) != pdPASS) break;
    }
    if (staging_allocated != RX_STAGING_COUNT) {
        for (size_t i = 0; i < RX_STAGING_COUNT; ++i) {
            if (s_rx_staging_buffers[i]) {
                heap_caps_free(s_rx_staging_buffers[i]);
                s_rx_staging_buffers[i] = nullptr;
            }
        }
        ESP_LOGE(TAG, "WS_RX: gagal alokasi transport staging pool");
        return false;
    }

    size_t process_allocated = 0;
    for (; process_allocated < RX_PROCESS_COUNT; ++process_allocated) {
        s_rx_process_buffers[process_allocated] = static_cast<char *>(
            heap_caps_malloc(RX_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rx_process_buffers[process_allocated]) break;
        if (xQueueSend(s_rx_process_free_queue, &s_rx_process_buffers[process_allocated], 0) != pdPASS) break;
    }
    if (process_allocated != RX_PROCESS_COUNT) {
        for (size_t i = 0; i < RX_PROCESS_COUNT; ++i) {
            if (s_rx_process_buffers[i]) {
                heap_caps_free(s_rx_process_buffers[i]);
                s_rx_process_buffers[i] = nullptr;
            }
        }
        for (size_t i = 0; i < RX_STAGING_COUNT; ++i) {
            if (s_rx_staging_buffers[i]) {
                heap_caps_free(s_rx_staging_buffers[i]);
                s_rx_staging_buffers[i] = nullptr;
            }
        }
        ESP_LOGE(TAG, "WS_RX: gagal alokasi processing pool");
        return false;
    }

    if (xTaskCreatePinnedToCore([](void *) {
            ESP_LOGI(TAG, "WS_RX: worker START staging=%u processing=%u size=%uKB priority=%u core=%d",
                     (unsigned)RX_STAGING_COUNT, (unsigned)RX_PROCESS_COUNT,
                     (unsigned)(RX_MAX_PAYLOAD / 1024U), (unsigned)RX_WORKER_PRIORITY, (int)RX_WORKER_CORE);
            int64_t last_metrics_us = 0;
            for (;;) {
                rx_item_t item = {};
                if (xQueueReceive(s_rx_ready_queue, &item, pdMS_TO_TICKS(RX_METRICS_PERIOD_MS)) != pdPASS) {
                    log_rx_metrics("periodic");
                    continue;
                }
                note_ready_highwater();
                if (!item.buffer || item.len == 0 || item.len > RX_MAX_PAYLOAD) {
                    if (item.buffer) (void)return_buffer(s_rx_staging_free_queue, &item.buffer, "staging");
                    metric_inc(&s_rx_payload_drops);
                    continue;
                }

                char *process_buffer = nullptr;
                if (xQueueReceive(s_rx_process_free_queue, &process_buffer, 0) != pdPASS || !process_buffer) {
                    metric_inc(&s_rx_payload_drops);
                    const uint32_t drops = __atomic_load_n(&s_rx_payload_drops, __ATOMIC_RELAXED);
                    if (drops - __atomic_load_n(&s_rx_last_starvation_log, __ATOMIC_RELAXED) >= 8U) {
                        __atomic_store_n(&s_rx_last_starvation_log, drops, __ATOMIC_RELAXED);
                        ESP_LOGW(TAG, "WS_RX: processing pool empty; payload dropped total=%lu", (unsigned long)drops);
                    }
                    (void)return_buffer(s_rx_staging_free_queue, &item.buffer, "staging");
                    continue;
                }
                note_process_free_depth();

                const int64_t copy_start_us = esp_timer_get_time();
                memcpy(process_buffer, item.buffer, item.len + 1U);
                const int64_t copy_done_us = esp_timer_get_time();
                (void)return_buffer(s_rx_staging_free_queue, &item.buffer, "staging");
                item.buffer = nullptr;

                const uint32_t current_generation = websocket_transport_generation();
                const int64_t process_start_us = esp_timer_get_time();
                if (item.generation != current_generation) {
                    ESP_LOGW(TAG, "WS_RX: stale message generation=%lu current=%lu dropped",
                             (unsigned long)item.generation, (unsigned long)current_generation);
                } else {
                    metric_inc(&s_rx_processing_count);
                    (void)gemini_protocol_process_message(process_buffer, item.len, item.generation);
                }
                const uint32_t processing_us = (uint32_t)(esp_timer_get_time() - process_start_us);
                metric_max(&s_rx_processing_max_us, processing_us);
                (void)copy_start_us;
                (void)copy_done_us;

                if (!return_buffer(s_rx_process_free_queue, &process_buffer, "processing")) {
                    ESP_LOGE(TAG, "WS_RX: processing buffer lost");
                }

                const int64_t now_us = esp_timer_get_time();
                if (!last_metrics_us || now_us - last_metrics_us >= 5000000LL) {
                    last_metrics_us = now_us;
                    log_rx_metrics("periodic");
                }
            }
        }, "ws_rx", RX_WORKER_STACK, nullptr, RX_WORKER_PRIORITY, &s_rx_worker_task, RX_WORKER_CORE) != pdPASS) {
        ESP_LOGE(TAG, "WS_RX: gagal membuat RX worker");
        return false;
    }

    s_rx_worker_ready = true;
    note_staging_depth();
    note_process_free_depth();
    return true;
}

static void reset_rx(void)
{
    if (s_rx_assembling_buffer) {
        (void)return_buffer(s_rx_staging_free_queue, &s_rx_assembling_buffer, "staging");
        s_rx_assembling_buffer = nullptr;
    }
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_first_opcode = 0;
    s_rx_generation = 0;
    s_rx_assembling = false;
    s_rx_discarding = false;
    s_rx_discard_expected = 0;
    s_rx_discard_received = 0;
    s_rx_discard_generation = 0;
    note_staging_depth();
}

static void begin_discard(size_t payload_len, uint32_t generation)
{
    s_rx_discarding = true;
    s_rx_discard_expected = payload_len;
    s_rx_discard_received = 0;
    s_rx_discard_generation = generation;
}

static void handle_data_event(esp_websocket_event_data_t *event)
{
    const int64_t callback_start_us = esp_timer_get_time();
    if (!event || !event->data_ptr || event->data_len <= 0 || event->payload_len <= 0) return;

    metric_inc(&s_rx_chunks);
    metric_add_u64(&s_rx_bytes, (uint64_t)event->data_len);

    const uint8_t raw_opcode = event->op_code;
    const uint8_t opcode = raw_opcode & 0x0FU;
    const size_t payload_len = (size_t)event->payload_len;
    const size_t payload_offset = (size_t)event->payload_offset;
    const size_t data_len = (size_t)event->data_len;

    if (payload_len > RX_MAX_PAYLOAD || payload_offset > payload_len || data_len > payload_len - payload_offset) {
        ESP_LOGW(TAG, "WS_RX: invalid boundary total=%u offset=%u len=%u", (unsigned)payload_len, (unsigned)payload_offset, (unsigned)data_len);
        reset_rx();
        metric_inc(&s_rx_fragment_errors);
        return;
    }
    if (opcode == 0x8U || opcode == 0x9U || opcode == 0xAU) return;
    if (opcode != 0x00U && opcode != 0x01U && opcode != 0x02U) {
        ESP_LOGW(TAG, "WS_RX: unsupported opcode=0x%02X dropped", opcode);
        reset_rx();
        metric_inc(&s_rx_fragment_errors);
        return;
    }
    if (!ensure_rx_worker()) {
        ESP_LOGW(TAG, "WS_RX: RX worker unavailable; payload dropped");
        reset_rx();
        metric_inc(&s_rx_payload_drops);
        return;
    }

    if (payload_offset == 0U) {
        reset_rx();
        if (opcode != 0x01U && opcode != 0x02U) {
            metric_inc(&s_rx_fragment_errors);
            return;
        }
        if (xQueueReceive(s_rx_staging_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) {
            metric_inc(&s_rx_payload_drops);
            begin_discard(payload_len, websocket_transport_generation());
            const uint32_t drops = __atomic_load_n(&s_rx_payload_drops, __ATOMIC_RELAXED);
            if (drops - __atomic_load_n(&s_rx_last_starvation_log, __ATOMIC_RELAXED) >= 8U) {
                __atomic_store_n(&s_rx_last_starvation_log, drops, __ATOMIC_RELAXED);
                ESP_LOGW(TAG, "WS_RX: staging pool empty; payload dropped total=%lu", (unsigned long)drops);
            }
            return;
        }
        note_staging_depth();
        s_rx_expected = payload_len;
        s_rx_received = 0;
        s_rx_first_opcode = opcode;
        s_rx_generation = websocket_transport_generation();
        s_rx_assembling = true;
    } else if (s_rx_discarding) {
        if (payload_len == s_rx_discard_expected && payload_offset == s_rx_discard_received) {
            s_rx_discard_received += data_len;
            if (s_rx_discard_received >= s_rx_discard_expected) {
                s_rx_discarding = false;
                s_rx_discard_expected = 0;
                s_rx_discard_received = 0;
                s_rx_discard_generation = 0;
            }
        } else {
            /* Ignore the remainder of a failed message. Do not cascade logs. */
        }
        metric_max(&s_rx_callback_max_us, (uint32_t)(esp_timer_get_time() - callback_start_us));
        return;
    } else {
        const bool opcode_matches_message = (opcode == 0x00U || opcode == s_rx_first_opcode);
        if (!s_rx_assembling || s_rx_expected != payload_len || payload_offset != s_rx_received || !opcode_matches_message) {
            metric_inc(&s_rx_fragment_errors);
            reset_rx();
            return;
        }
    }

    memcpy(s_rx_assembling_buffer + payload_offset, event->data_ptr, data_len);
    s_rx_received = payload_offset + data_len;

    if (s_rx_received != s_rx_expected) {
        metric_max(&s_rx_callback_max_us, (uint32_t)(esp_timer_get_time() - callback_start_us));
        return;
    }

    s_rx_assembling_buffer[s_rx_expected] = '\0';
    rx_item_t item = {s_rx_assembling_buffer, s_rx_expected, s_rx_generation};
    s_rx_assembling_buffer = nullptr;
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_first_opcode = 0;
    s_rx_generation = 0;
    s_rx_assembling = false;

    if (xQueueSend(s_rx_ready_queue, &item, 0) != pdPASS) {
        metric_inc(&s_rx_payload_drops);
        (void)return_buffer(s_rx_staging_free_queue, &item.buffer, "staging");
        ESP_LOGW(TAG, "WS_RX: ready queue penuh; payload dropped");
    } else {
        note_ready_highwater();
    }
    metric_max(&s_rx_callback_max_us, (uint32_t)(esp_timer_get_time() - callback_start_us));
}

void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        websocket_transport_event_connected(); websocket_event_reset(); (void)ensure_rx_worker();
        audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED, websocket_transport_generation());
        if (!gemini_protocol_on_connected()) { ESP_LOGE(TAG, "Gemini setup send failed"); (void)websocket_transport_disconnect(); }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (websocket_transport_is_connected()) handle_data_event(static_cast<esp_websocket_event_data_t *>(event_data));
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        websocket_audio_on_disconnected(); audio_engine_stop_input_session(); gemini_protocol_on_disconnected(); reset_rx(); websocket_transport_event_disconnected(); break;
    case WEBSOCKET_EVENT_ERROR: ESP_LOGE(TAG, "WebSocket ERROR"); break;
    default: break;
    }
}

void websocket_event_reset(void) { reset_rx(); }
bool websocket_event_gemini_ready(void) { return gemini_protocol_setup_complete(); }
