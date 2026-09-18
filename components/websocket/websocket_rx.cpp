#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_RX";
static volatile bool ws_rx_reset_pending = false;
static uint8_t *rx_slots[WS_RX_SLOT_COUNT] = {0};
static size_t rx_slot_capacity[WS_RX_SLOT_COUNT] = {0};
static bool rx_slot_in_use[WS_RX_SLOT_COUNT] = {false};
static bool rx_slot_psram[WS_RX_SLOT_COUNT] = {false};
static bool rx_slots_preallocated = false;
static int rx_capture_slot = -1;
static size_t ws_rx_received = 0;
static size_t ws_rx_expected = 0;
static bool ws_rx_active = false;
static bool ws_rx_streaming = false;
static size_t rx_stream_compact_len = 0;
static bool rx_stream_in_inline = false;
static uint8_t rx_stream_inline_match = 0;
static uint8_t rx_stream_data_match = 0;
static uint8_t rx_stream_state = 0;
static char rx_stream_b64_quad[4];
static uint8_t rx_stream_b64_len = 0;
static uint8_t rx_stream_pcm[1024];
static size_t rx_stream_pcm_len = 0;
static bool rx_stream_audio_seen = false;
static bool rx_stream_failed = false;
static const char RX_INLINE_TOKEN[] = "\"inlineData\"";
static const char RX_DATA_TOKEN[] = "\"data\"";
static size_t ws_rx_received_full = 0;
static size_t ws_rx_expected_full = 0;
static uint32_t rx_fragments_received = 0;
static uint32_t rx_fragments_dropped = 0;
static uint32_t rx_complete_messages = 0;
static UBaseType_t rx_queue_high_water = 0;
static uint32_t rx_sequence_errors = 0;
static uint32_t rx_buffer_drops = 0;
static uint32_t rx_queue_drops = 0;
static uint32_t rx_invalid_json = 0;
static uint32_t rx_oversize_drops = 0;
static uint32_t rx_largest_payload = 0;
static size_t rx_drop_payload_len = 0;
static size_t rx_drop_received = 0;

static size_t rx_capacity_for(size_t required)
{
    if (required == 0 || required > WS_RX_SLOT_SIZE) return 0;
    static const size_t buckets[] = { 5120, 8192, 12288, 16384, 20480, 24576, 32768, 40960 };
    for (size_t i = 0; i < sizeof(buckets) / sizeof(buckets[0]); ++i)
        if (required <= buckets[i]) return buckets[i];
    return 0;
}

static void release_slot(uint8_t slot_id)
{
    if (slot_id < WS_RX_SLOT_COUNT) rx_slot_in_use[slot_id] = false;
}

static int reserve_slot(void)
{
    for (int i = 0; i < WS_RX_SLOT_COUNT; ++i) {
        if (!rx_slot_in_use[i]) { rx_slot_in_use[i] = true; return i; }
    }
    return -1;
}

/* Alokasi slot hanya dipakai saat prealokasi, tidak saat streaming */
static bool allocate_persistent_slot(int slot, size_t target)
{
    if (slot < 0 || slot >= WS_RX_SLOT_COUNT || target == 0) return false;

    uint8_t *p = NULL;
    bool psram = false;

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        p = (uint8_t *)heap_caps_malloc(target, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        psram = (p != NULL);
    }
    if (!p) p = (uint8_t *)heap_caps_malloc(target, MALLOC_CAP_8BIT);
    if (!p) return false;

    if (rx_slots[slot]) heap_caps_free(rx_slots[slot]);
    rx_slots[slot] = p;
    rx_slot_capacity[slot] = target;
    rx_slot_psram[slot] = psram;
    ESP_LOGI(TAG, "RX slot persistent: slot=%d capacity=%u memory=%s",
             slot, (unsigned)target, psram ? "PSRAM" : "internal");
    return true;
}

static bool preallocate_rx_slots(void)
{
    if (rx_slots_preallocated) return true;

    // Semua slot 8 KB — cukup untuk compact JSON dan payload kecil
    for (int i = 0; i < WS_RX_SLOT_COUNT; ++i) {
        if (!allocate_persistent_slot(i, 8 * 1024)) {
            ESP_LOGE(TAG, "Prealokasi slot gagal: slot=%d", i);
            return false;
        }
        release_slot(i);
    }

    rx_slots_preallocated = true;
    ESP_LOGI(TAG, "Prealokasi RX slot selesai: %dx8KB", (unsigned)WS_RX_SLOT_COUNT);
    return true;
}

static bool ensure_slot_buffer(int slot, size_t required_size)
{
    if (slot < 0 || slot >= WS_RX_SLOT_COUNT || required_size == 0) return false;
    if (required_size > WS_RX_SLOT_SIZE - WS_RX_TERMINATOR_SIZE) return false;
    const size_t needed = required_size + WS_RX_TERMINATOR_SIZE;

    if (rx_slots[slot] && rx_slot_capacity[slot] >= needed) return true;

    ESP_LOGE(TAG, "Slot %d tidak siap: cap=%u needed=%u",
             slot, (unsigned)rx_slot_capacity[slot], (unsigned)needed);
    return false;
}

static void free_rx_command(ws_rx_command_t *cmd)
{
    if (!cmd) return;
    release_slot(cmd->slot_id);
    cmd->buffer = NULL;
}

static void log_rx_stats(const char *reason, uint32_t message_len, uint32_t process_ms,
                         size_t heap_before, size_t heap_after,
                         size_t largest_before, size_t largest_after)
{
    ESP_LOGI(TAG,
             "RX STATS [%s]: fragments=%lu dropped_frag=%lu messages=%lu queue_hwm=%u seq_err=%lu buffer_drop=%lu queue_drop=%lu invalid=%lu oversize=%lu max_payload=%lu",
             reason, (unsigned long)rx_fragments_received,
             (unsigned long)rx_fragments_dropped, (unsigned long)rx_complete_messages,
             (unsigned)rx_queue_high_water, (unsigned long)rx_sequence_errors,
             (unsigned long)rx_buffer_drops, (unsigned long)rx_queue_drops,
             (unsigned long)rx_invalid_json, (unsigned long)rx_oversize_drops,
             (unsigned long)rx_largest_payload);
    ESP_LOGI(TAG, "RX PROCESS: len=%lu time=%lu ms heap=%u->%u largest=%u->%u",
             (unsigned long)message_len, (unsigned long)process_ms,
             (unsigned)heap_before, (unsigned)heap_after,
             (unsigned)largest_before, (unsigned)largest_after);
}

static bool stream_append_char(char c)
{
    if (rx_stream_compact_len + 1 >= WS_RX_STREAM_COMPACT_SIZE) {
        if (!rx_stream_failed)
            ESP_LOGW(TAG, "RX stream compact JSON overflow: cap=%u", (unsigned)WS_RX_STREAM_COMPACT_SIZE);
        rx_stream_failed = true;
        return false;
    }
    rx_slots[rx_capture_slot][rx_stream_compact_len++] = (uint8_t)c;
    return true;
}

static bool stream_flush_pcm(void)
{
    if (rx_stream_pcm_len == 0)
        return true;

    size_t n = rx_stream_pcm_len;
    uint8_t sisa = 0;
    bool has_sisa = false;

    if (n & 1) {
        sisa = rx_stream_pcm[n - 1];
        n--;
        has_sisa = true;
    }

    if (n == 0) {
        rx_stream_pcm[0] = sisa;
        rx_stream_pcm_len = 1;
        return true;
    }

    audio_bytes_received += n;
    audio_chunks_received++;

    bool ok = queue_audio_pcm(rx_stream_pcm, n);

    if (has_sisa) {
        rx_stream_pcm[0] = sisa;
        rx_stream_pcm_len = 1;
    } else {
        rx_stream_pcm_len = 0;
    }

    if (!ok)
        ESP_LOGW(TAG,
                 "AUDIO STREAM: queue PCM gagal len=%u",
                 (unsigned)n);

    return ok;
}

static bool stream_decode_quad(void)
{
    size_t out_len = 3;
    uint8_t out[3];
    int ret = mbedtls_base64_decode(out, sizeof(out), &out_len,
                                    (const unsigned char *)rx_stream_b64_quad, 4);
    if (ret != 0) {
        ESP_LOGW(TAG, "AUDIO STREAM: base64 quad gagal ret=-0x%04X", -ret);
        rx_stream_failed = true;
        return false;
    }
    if (out_len > 0) {
        memcpy(rx_stream_pcm + rx_stream_pcm_len, out, out_len);
        rx_stream_pcm_len += out_len;
        if (rx_stream_pcm_len >= sizeof(rx_stream_pcm) - 3)
            return stream_flush_pcm();
    }
    return true;
}

static bool stream_feed_base64_char(char c)
{
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
        ESP_LOGW(TAG, "AUDIO STREAM: karakter base64 tidak valid 0x%02X", (unsigned char)c);
        rx_stream_failed = true;
        return false;
    }
    rx_stream_b64_quad[rx_stream_b64_len++] = c;
    if (rx_stream_b64_len == 4) {
        bool ok = stream_decode_quad();
        rx_stream_b64_len = 0;
        return ok;
    }
    return true;
}

static void stream_reset_state(void)
{
    rx_stream_compact_len = 0;
    rx_stream_in_inline = false;
    rx_stream_inline_match = 0;
    rx_stream_data_match = 0;
    rx_stream_state = 0;
    rx_stream_b64_len = 0;
    rx_stream_pcm_len = 0;
    rx_stream_audio_seen = false;
    rx_stream_failed = false;
}

static bool stream_finish(void)
{
    if (rx_stream_state == 3 && rx_stream_b64_len > 0) {
        if (rx_stream_b64_len == 1) {
            ESP_LOGW(TAG, "AUDIO STREAM: base64 tail tidak valid");
            rx_stream_failed = true;
        } else {
            while (rx_stream_b64_len < 4) rx_stream_b64_quad[rx_stream_b64_len++] = '=';
            stream_decode_quad();
            rx_stream_b64_len = 0;
        }
    }
    stream_flush_pcm();
    if (rx_stream_compact_len + 1 < WS_RX_STREAM_COMPACT_SIZE)
        rx_slots[rx_capture_slot][rx_stream_compact_len] = '\0';
    return rx_stream_audio_seen && !rx_stream_failed;
}

static bool stream_consume_fragment(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || rx_capture_slot < 0) return false;

    for (size_t i = 0; i < len; ++i) {
        const char c = (char)data[i];

        if (rx_stream_state == 3) {
            if (c == '"') {
                stream_flush_pcm();
                rx_stream_state = 0;
                rx_stream_in_inline = false;
                rx_stream_b64_len = 0;
                continue;
            }
            if (!stream_feed_base64_char(c)) return false;
            rx_stream_audio_seen = true;
            continue;
        }

        if (rx_stream_state == 1) {
            if (c == ':') {
                if (!stream_append_char(c)) return false;
                rx_stream_state = 2;
            } else {
                if (!stream_append_char(c)) return false;
            }
            continue;
        }

        if (rx_stream_state == 2) {
            if (c == '"') {
                if (!stream_append_char('"')) return false;
                if (!stream_append_char('"')) return false;
                rx_stream_state = 3;
                rx_stream_audio_seen = true;
            } else {
                if (!stream_append_char(c)) return false;
            }
            continue;
        }

        if (!stream_append_char(c)) return false;

        if (!rx_stream_in_inline) {
            if (c == RX_INLINE_TOKEN[rx_stream_inline_match]) {
                ++rx_stream_inline_match;
                if (rx_stream_inline_match == sizeof(RX_INLINE_TOKEN) - 1) {
                    rx_stream_in_inline = true;
                    rx_stream_inline_match = 0;
                    rx_stream_data_match = 0;
                }
            } else {
                rx_stream_inline_match = (c == RX_INLINE_TOKEN[0]) ? 1 : 0;
            }
        } else {
            if (c == RX_DATA_TOKEN[rx_stream_data_match]) {
                ++rx_stream_data_match;
                if (rx_stream_data_match == sizeof(RX_DATA_TOKEN) - 1) {
                    rx_stream_data_match = 0;
                    rx_stream_state = 1;
                }
            } else {
                rx_stream_data_match = (c == RX_DATA_TOKEN[0]) ? 1 : 0;
            }
        }
    }
    return true;
}

static void stream_discard_message(size_t payload_len, size_t received)
{
    rx_drop_payload_len = payload_len;
    rx_drop_received = received;
    if (rx_drop_received >= rx_drop_payload_len) {
        rx_drop_payload_len = 0;
        rx_drop_received = 0;
    }
}

static void stream_queue_compact_message(uint32_t generation)
{
    if (!rx_stream_audio_seen || rx_stream_failed || rx_stream_compact_len == 0) return;
    ws_rx_command_t cmd = {};
    cmd.generation = generation;
    cmd.buffer = rx_slots[rx_capture_slot];
    cmd.len = (uint32_t)rx_stream_compact_len;
    cmd.slot_id = (uint8_t)rx_capture_slot;
    if (xQueueSend(websocket_rx_queue, &cmd, 0) != pdTRUE) {
        ++rx_fragments_dropped;
        ++rx_queue_drops;
        release_slot(cmd.slot_id);
        ESP_LOGW(TAG, "RX QUEUE DROP: streamed payload=%u", (unsigned)cmd.len);
        return;
    }
    UBaseType_t waiting = uxQueueMessagesWaiting(websocket_rx_queue);
    if (waiting > rx_queue_high_water) rx_queue_high_water = waiting;
    rx_capture_slot = -1;
}

static void websocket_rx_task(void *arg)
{
    (void)arg;
    ws_rx_command_t cmd = {};
    ESP_LOGI(TAG, "WebSocket RX worker dimulai - persistent slot pool + audio stream");
    for (;;) {
        if (xQueueReceive(websocket_rx_queue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (ws_rx_reset_pending) { ws_rx_reset_pending = false; reset_rx_buffer(); }
            continue;
        }
        if (ws_rx_reset_pending) { ws_rx_reset_pending = false; reset_rx_buffer(); }
        if (!cmd.buffer || cmd.len == 0) {
            release_slot(cmd.slot_id); memset(&cmd, 0, sizeof(cmd)); continue;
        }
        if (cmd.generation != websocket_connection_generation || !is_connected) {
            free_rx_command(&cmd); memset(&cmd, 0, sizeof(cmd)); continue;
        }
        size_t heap_before = esp_get_free_heap_size();
        size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        int64_t process_start = esp_timer_get_time();
        process_gemini_message((const char *)cmd.buffer, (size_t)cmd.len);
        uint32_t process_ms = (uint32_t)((esp_timer_get_time() - process_start) / 1000);
        size_t heap_after = esp_get_free_heap_size();
        size_t largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ++rx_complete_messages;
        log_rx_stats("processed", cmd.len, process_ms, heap_before, heap_after,
                     largest_before, largest_after);
        free_rx_command(&cmd);
        memset(&cmd, 0, sizeof(cmd));
    }
}

bool websocket_rx_init(void)
{
    if (!websocket_rx_queue) {
        websocket_rx_queue = xQueueCreate(WS_RX_QUEUE_LENGTH, sizeof(ws_rx_command_t));
        if (!websocket_rx_queue) { ESP_LOGE(TAG, "Gagal membuat RX queue"); return false; }
    }

    if (!preallocate_rx_slots()) {
        ESP_LOGE(TAG, "Prealokasi slot RX gagal, init dibatalkan");
        return false;
    }

    if (!websocket_rx_task_handle) {
        if (xTaskCreate(websocket_rx_task, "ws_rx", 8192, NULL, 5,
                        &websocket_rx_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat RX worker"); return false;
        }
    }
    return true;
}

void websocket_rx_request_reset(void) { ws_rx_reset_pending = true; }

void websocket_rx_flush_queue(void)
{
    if (!websocket_rx_queue) return;
    ws_rx_command_t stale = {};
    size_t flushed = 0;
    while (xQueueReceive(websocket_rx_queue, &stale, 0) == pdTRUE) {
        release_slot(stale.slot_id);
        ++flushed;
    }
    if (flushed) ESP_LOGW(TAG, "RX queue dibersihkan: %u message", (unsigned)flushed);
}

void websocket_rx_note_invalid_json(size_t len)
{
    ++rx_invalid_json;
    ESP_LOGW(TAG, "RX INVALID JSON #%lu: len=%u", (unsigned long)rx_invalid_json, (unsigned)len);
}

bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data, uint32_t generation)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0 ||
        data->payload_offset < 0 || generation != websocket_connection_generation || !is_connected) {
        ++rx_fragments_dropped; return false;
    }
    ++rx_fragments_received;
    const size_t payload_len = (size_t)data->payload_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t len = (size_t)data->data_len;
    if (payload_len > rx_largest_payload) rx_largest_payload = (uint32_t)payload_len;
    if (offset > payload_len || len > payload_len - offset) {
        ++rx_fragments_dropped; ++rx_sequence_errors; return false;
    }

    if (rx_drop_payload_len != 0) {
        if (offset == 0 && rx_drop_received != 0) {
            ++rx_sequence_errors; rx_drop_payload_len = 0; rx_drop_received = 0;
        } else {
            ++rx_fragments_dropped;
            if (offset == rx_drop_received) rx_drop_received += len;
            else { ++rx_sequence_errors; rx_drop_received = offset + len; }
            if (rx_drop_received >= rx_drop_payload_len) { rx_drop_payload_len = 0; rx_drop_received = 0; }
            return true;
        }
    }

    if (offset == 0) {
        if (ws_rx_active) { ++rx_fragments_dropped; ++rx_sequence_errors; reset_rx_buffer(); }

        ws_rx_streaming = payload_len > WS_RX_STREAM_THRESHOLD;

        if (payload_len > WS_RX_MAX_PAYLOAD_SIZE) {
            ++rx_fragments_dropped; ++rx_oversize_drops;
            stream_discard_message(payload_len, len);
            ESP_LOGW(TAG, "RX oversize: payload=%u limit=%u", (unsigned)payload_len,
                     (unsigned)WS_RX_MAX_PAYLOAD_SIZE);
            return false;
        }

        int slot = reserve_slot();
        if (slot < 0) {
            ++rx_fragments_dropped; ++rx_buffer_drops;
            stream_discard_message(payload_len, len);
            ESP_LOGW(TAG, "RX BUFFER DROP: no free slot payload=%u", (unsigned)payload_len);
            return false;
        }

        

        size_t required = ws_rx_streaming ? (WS_RX_STREAM_COMPACT_SIZE - 1) : payload_len;
        if (!ensure_slot_buffer(slot, required)) {
            release_slot((uint8_t)slot);
            ++rx_fragments_dropped; ++rx_buffer_drops;
            stream_discard_message(payload_len, len);
            ESP_LOGW(TAG, "RX slot allocation gagal: payload=%u stream=%s", (unsigned)payload_len,
                     ws_rx_streaming ? "yes" : "no");
            return false;
        }

        rx_capture_slot = slot;
        ws_rx_received = 0;
        ws_rx_expected = payload_len;
        ws_rx_received_full = 0;
        ws_rx_expected_full = payload_len;
        ws_rx_active = true;

        if (ws_rx_streaming) {
            stream_reset_state();
            if (!stream_consume_fragment((const uint8_t *)data->data_ptr, len)) {
                ++rx_fragments_dropped;
                ++rx_buffer_drops;
                stream_discard_message(payload_len, len);
                reset_rx_buffer();
                return false;
            }
            ws_rx_received_full = offset + len;
        }
    } else if (!ws_rx_active || rx_capture_slot < 0 || rx_capture_slot >= WS_RX_SLOT_COUNT ||
               ws_rx_expected_full != payload_len || offset != ws_rx_received_full) {
        ++rx_fragments_dropped; ++rx_sequence_errors; reset_rx_buffer(); return false;
    }

    if (!ws_rx_streaming) {
        if (!ws_rx_active || rx_capture_slot < 0 || rx_capture_slot >= WS_RX_SLOT_COUNT ||
            !rx_slots[rx_capture_slot] || ws_rx_expected != payload_len || offset != ws_rx_received) {
            ++rx_fragments_dropped; ++rx_sequence_errors; reset_rx_buffer(); return false;
        }
        memcpy(rx_slots[rx_capture_slot] + offset, data->data_ptr, len);
        ws_rx_received = offset + len;
        ws_rx_received_full = ws_rx_received;
    } else if (offset != 0) {
        if (!stream_consume_fragment((const uint8_t *)data->data_ptr, len)) {
            ++rx_fragments_dropped;
            ++rx_buffer_drops;
            stream_discard_message(payload_len, offset + len);
            reset_rx_buffer();
            return false;
        }
        ws_rx_received_full = offset + len;
    }

    if (ws_rx_received_full == ws_rx_expected_full) {
        if (ws_rx_streaming) {
            const bool ok = stream_finish();
            if (ok) {
                stream_queue_compact_message(generation);
            } else {
                ++rx_fragments_dropped;
                ++rx_buffer_drops;
                ESP_LOGW(TAG, "RX streamed audio message discarded: compact=%u audio_seen=%s failed=%s",
                         (unsigned)rx_stream_compact_len,
                         rx_stream_audio_seen ? "yes" : "no",
                         rx_stream_failed ? "yes" : "no");
                release_slot((uint8_t)rx_capture_slot);
                rx_capture_slot = -1;
            }
            ws_rx_streaming = false;
            ws_rx_active = false;
            ws_rx_received_full = 0;
            ws_rx_expected_full = 0;
            return ok;
        }

        rx_slots[rx_capture_slot][ws_rx_expected] = '\0';
        ws_rx_active = false;
        ws_rx_command_t cmd = {};
        cmd.generation = generation;
        cmd.buffer = rx_slots[rx_capture_slot];
        cmd.len = (uint32_t)ws_rx_expected;
        cmd.slot_id = (uint8_t)rx_capture_slot;
        if (xQueueSend(websocket_rx_queue, &cmd, 0) != pdTRUE) {
            ++rx_fragments_dropped; ++rx_queue_drops; release_slot(cmd.slot_id);
            ESP_LOGW(TAG, "RX QUEUE DROP: payload=%u", (unsigned)cmd.len);
            ws_rx_received = 0; ws_rx_expected = 0; rx_capture_slot = -1;
            return false;
        }
        UBaseType_t waiting = uxQueueMessagesWaiting(websocket_rx_queue);
        if (waiting > rx_queue_high_water) rx_queue_high_water = waiting;
        ws_rx_received = 0; ws_rx_expected = 0; rx_capture_slot = -1;
    }
    return true;
}

void reset_rx_buffer(void)
{
    if (rx_capture_slot >= 0 && rx_capture_slot < WS_RX_SLOT_COUNT)
        release_slot((uint8_t)rx_capture_slot);
    ws_rx_active = false;
    ws_rx_streaming = false;
    ws_rx_received = 0;
    ws_rx_expected = 0;
    ws_rx_received_full = 0;
    ws_rx_expected_full = 0;
    rx_capture_slot = -1;
    rx_drop_payload_len = 0;
    rx_drop_received = 0;
    stream_reset_state();
}

bool ensure_rx_buffer(size_t required_size)
{
    return required_size > 0 && required_size <= WS_RX_MAX_PAYLOAD_SIZE &&
           required_size <= WS_RX_SLOT_SIZE - WS_RX_TERMINATOR_SIZE;
}

void process_websocket_payload(esp_websocket_event_data_t *data)
{
    if (!data) return;
    websocket_rx_enqueue_data(data, websocket_connection_generation);
}
