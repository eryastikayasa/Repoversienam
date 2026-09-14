#include "websocket_audio.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";
static constexpr size_t MIC_FRAME_BYTES = 640U;
static constexpr size_t MIC_TX_CHUNK_BYTES = 1600U;
static constexpr size_t MIC_TX_ACCUMULATOR_BYTES = 3200U;
static constexpr size_t MIC_NET_QUEUE_DEPTH = 6U;
static constexpr uint32_t MIC_NET_TX_STACK = 6144U;
static constexpr UBaseType_t MIC_NET_TX_PRIORITY = 4U;
static constexpr BaseType_t MIC_NET_TX_CORE = 0;

struct mic_net_item_t { uint32_t generation; uint8_t pcm[MIC_FRAME_BYTES]; };
static StaticQueue_t s_mic_net_queue_struct;
static uint8_t s_mic_net_queue_storage[MIC_NET_QUEUE_DEPTH * sizeof(mic_net_item_t)];
static QueueHandle_t s_mic_net_queue = nullptr;
static TaskHandle_t s_mic_net_task = nullptr;
static uint32_t s_mic_net_queue_drops = 0;
static uint32_t s_mic_net_queue_high = 0;
static uint32_t s_send_ok = 0;
static uint32_t s_send_timeout = 0;
static uint32_t s_send_return_zero = 0;
static uint32_t s_send_transport_error = 0;
static uint32_t s_send_invalid_state = 0;
static uint8_t s_mic_tx_accumulator[MIC_TX_ACCUMULATOR_BYTES];
static size_t s_mic_tx_accumulated = 0;
static char s_audio_b64[2300];
static char s_audio_json[2500];
static uint64_t s_profile_last_us = 0, s_profile_count = 0;
static uint64_t s_profile_encode_us = 0, s_profile_encode_max_us = 0;
static uint64_t s_profile_json_us = 0, s_profile_json_max_us = 0;
static uint64_t s_profile_poll_us = 0, s_profile_poll_max_us = 0;
static uint64_t s_profile_tls_us = 0, s_profile_tls_max_us = 0;
static uint64_t s_profile_transport_us = 0, s_profile_transport_max_us = 0;
static uint64_t s_profile_send_us = 0, s_profile_send_max_us = 0;
static uint64_t s_profile_total_us = 0, s_profile_total_max_us = 0;
static uint32_t s_send_diag_frames = 0;

static void drain_mic_net_queue(void)
{
    if (!s_mic_net_queue) return;
    mic_net_item_t item = {};
    while (xQueueReceive(s_mic_net_queue, &item, 0) == pdTRUE) {}
    s_mic_tx_accumulated = 0;
}

static void log_mic_net_stack(const char *stage)
{
    if (!s_mic_net_task) return;
    ESP_LOGI(TAG, "TASK AUDIT mic_net_tx stage=%s stack=%uB watermark=%uB priority=%u core=%d",
             stage ? stage : "unknown", (unsigned)MIC_NET_TX_STACK,
             (unsigned)(uxTaskGetStackHighWaterMark(s_mic_net_task) * sizeof(StackType_t)),
             (unsigned)uxTaskPriorityGet(s_mic_net_task), (int)xTaskGetCoreID(s_mic_net_task));
}

static void profile_record(uint32_t encode_us, uint32_t json_us, uint32_t poll_us,
                           uint32_t tls_us, uint32_t transport_us, uint32_t send_us,
                           uint32_t total_us)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    ++s_profile_count;
    s_profile_encode_us += encode_us; s_profile_json_us += json_us;
    s_profile_poll_us += poll_us; s_profile_tls_us += tls_us;
    s_profile_transport_us += transport_us; s_profile_send_us += send_us;
    s_profile_total_us += total_us;
    if (encode_us > s_profile_encode_max_us) s_profile_encode_max_us = encode_us;
    if (json_us > s_profile_json_max_us) s_profile_json_max_us = json_us;
    if (poll_us > s_profile_poll_max_us) s_profile_poll_max_us = poll_us;
    if (tls_us > s_profile_tls_max_us) s_profile_tls_max_us = tls_us;
    if (transport_us > s_profile_transport_max_us) s_profile_transport_max_us = transport_us;
    if (send_us > s_profile_send_max_us) s_profile_send_max_us = send_us;
    if (total_us > s_profile_total_max_us) s_profile_total_max_us = total_us;
    if (!s_profile_last_us) s_profile_last_us = now_us;
    if (s_mic_net_queue) {
        const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(s_mic_net_queue);
        if (depth > s_mic_net_queue_high) s_mic_net_queue_high = depth;
    }
    if (now_us - s_profile_last_us < 5000000ULL) return;
    const uint64_t count = s_profile_count;
    ESP_LOGI(TAG,
        "MIC_TX_PROFILE: count=%llu encode_avg_us=%llu encode_max_us=%llu json_avg_us=%llu json_max_us=%llu poll_write_avg_us=%llu poll_write_max_us=%llu tls_write_avg_us=%llu tls_write_max_us=%llu transport_write_avg_us=%llu transport_write_max_us=%llu send_avg_us=%llu send_max_us=%llu total_avg_us=%llu total_max_us=%llu net_queue_high=%u net_queue_drops=%u send_ok=%u send_timeout=%u send_return_zero=%u send_transport_error=%u send_invalid_state=%u net_stack_watermark=%uB",
        (unsigned long long)count,
        (unsigned long long)(s_profile_encode_us / count), (unsigned long long)s_profile_encode_max_us,
        (unsigned long long)(s_profile_json_us / count), (unsigned long long)s_profile_json_max_us,
        (unsigned long long)(s_profile_poll_us / count), (unsigned long long)s_profile_poll_max_us,
        (unsigned long long)(s_profile_tls_us / count), (unsigned long long)s_profile_tls_max_us,
        (unsigned long long)(s_profile_transport_us / count), (unsigned long long)s_profile_transport_max_us,
        (unsigned long long)(s_profile_send_us / count), (unsigned long long)s_profile_send_max_us,
        (unsigned long long)(s_profile_total_us / count), (unsigned long long)s_profile_total_max_us,
        (unsigned)s_mic_net_queue_high, (unsigned)s_mic_net_queue_drops,
        (unsigned)s_send_ok, (unsigned)s_send_timeout, (unsigned)s_send_return_zero,
        (unsigned)s_send_transport_error, (unsigned)s_send_invalid_state,
        s_mic_net_task ? (unsigned)(uxTaskGetStackHighWaterMark(s_mic_net_task) * sizeof(StackType_t)) : 0U);
    s_profile_last_us = now_us; s_profile_count = 0;
    s_profile_encode_us = s_profile_encode_max_us = 0;
    s_profile_json_us = s_profile_json_max_us = 0;
    s_profile_poll_us = s_profile_poll_max_us = 0;
    s_profile_tls_us = s_profile_tls_max_us = 0;
    s_profile_transport_us = s_profile_transport_max_us = 0;
    s_profile_send_us = s_profile_send_max_us = 0;
    s_profile_total_us = s_profile_total_max_us = 0;
    s_mic_net_queue_high = 0; s_mic_net_queue_drops = 0;
    s_send_ok = s_send_timeout = s_send_return_zero = 0;
    s_send_transport_error = s_send_invalid_state = 0;
    log_mic_net_stack("profile");
}

static bool send_frame_network(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > MIC_TX_CHUNK_BYTES || !websocket_transport_is_connected() ||
        !gemini_protocol_setup_complete() || !gemini_protocol_greeting_finished()) return false;
    const int64_t total_start = esp_timer_get_time(), encode_start = total_start;
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)s_audio_b64, sizeof(s_audio_b64) - 1,
                              &b64_len, data, len) != 0) return false;
    s_audio_b64[b64_len] = '\0';
    const uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_start);
    const int64_t json_start = esp_timer_get_time();
    const int n = snprintf(s_audio_json, sizeof(s_audio_json),
        "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", s_audio_b64);
    const uint32_t json_us = (uint32_t)(esp_timer_get_time() - json_start);
    if (n <= 0 || (size_t)n >= sizeof(s_audio_json)) return false;

    uint64_t poll_before = 0, tls_before = 0, transport_before = 0;
    int last_poll_before = 0, last_transport_before = 0; ssize_t last_tls_before = 0;
    websocket_transport_profile_snapshot(&poll_before, &tls_before, &transport_before,
                                         &last_poll_before, &last_transport_before, &last_tls_before);
    const uint32_t diag_no = s_send_diag_frames++;
    if (diag_no < 3U) {
        ESP_LOGI(TAG, "MIC_NET_TX SEND_BEGIN frame=%u pcm=%uB generation=%lu connected=%d client=%p client_connected=%d setup=%d greeting=%d",
                 (unsigned)diag_no, (unsigned)len, (unsigned long)websocket_transport_generation(),
                 websocket_transport_is_connected() ? 1 : 0, (void *)websocket_transport_client(),
                 websocket_transport_client() && esp_websocket_client_is_connected(websocket_transport_client()) ? 1 : 0,
                 gemini_protocol_setup_complete() ? 1 : 0, gemini_protocol_greeting_finished() ? 1 : 0);
    }
    const int64_t send_start = esp_timer_get_time();
    const esp_err_t send_result = websocket_transport_send_text(s_audio_json, (size_t)n);
    const uint32_t send_us = (uint32_t)(esp_timer_get_time() - send_start);
    uint64_t poll_after = 0, tls_after = 0, transport_after = 0;
    int last_poll_after = 0, last_transport_after = 0; ssize_t last_tls_after = 0;
    websocket_transport_profile_snapshot(&poll_after, &tls_after, &transport_after,
                                         &last_poll_after, &last_transport_after, &last_tls_after);
    const uint32_t total_us = (uint32_t)(esp_timer_get_time() - total_start);
    profile_record(encode_us, json_us, (uint32_t)(poll_after - poll_before),
                   (uint32_t)(tls_after - tls_before), (uint32_t)(transport_after - transport_before),
                   send_us, total_us);
    if (send_result == ESP_OK) { ++s_send_ok; return true; }

    ESP_LOGW(TAG, "MIC_NET_TX SEND_FAIL result=%s elapsed_us=%u generation=%lu transport_connected=%d client=%p client_connected=%d poll_ret=%d transport_ret=%d tls_ret=%ld poll_delta_us=%u transport_delta_us=%u tls_delta_us=%u",
             esp_err_to_name(send_result), (unsigned)send_us, (unsigned long)websocket_transport_generation(),
             websocket_transport_is_connected() ? 1 : 0, (void *)websocket_transport_client(),
             websocket_transport_client() && esp_websocket_client_is_connected(websocket_transport_client()) ? 1 : 0,
             last_poll_after, last_transport_after, (long)last_tls_after,
             (unsigned)(poll_after - poll_before), (unsigned)(transport_after - transport_before),
             (unsigned)(tls_after - tls_before));
    if (send_result == ESP_ERR_TIMEOUT) {
        ++s_send_timeout; if (last_poll_after == 0 || last_transport_after == 0) ++s_send_return_zero;
        ESP_LOGW(TAG, "MIC_NET_TX: SEND_TIMEOUT; stopping MIC session, preserving WebSocket");
    } else if (send_result == ESP_ERR_INVALID_STATE) {
        ++s_send_invalid_state;
        ESP_LOGW(TAG, "MIC_NET_TX: SEND_INVALID_STATE; stopping MIC session");
    } else {
        ++s_send_transport_error;
        ESP_LOGW(TAG, "MIC_NET_TX: SEND_TRANSPORT_ERROR; stopping MIC session, preserving WebSocket");
    }
    audio_engine_stop_input_session();
    bool stopped = audio_engine_stop_capture_and_wait();
    if (!stopped) stopped = audio_engine_stop_capture_and_wait();
    if (!stopped) ESP_LOGE(TAG, "MIC_NET_TX: capture cleanup did not complete within bounded wait");
    return false;
}

static void mic_network_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MIC network sender START queue=%u frame=%uB batch=%uB accumulator=%uB stack=%uB timeout=3000ms retry=1 delay=30ms",
             (unsigned)MIC_NET_QUEUE_DEPTH, (unsigned)MIC_FRAME_BYTES,
             (unsigned)MIC_TX_CHUNK_BYTES, (unsigned)MIC_TX_ACCUMULATOR_BYTES,
             (unsigned)MIC_NET_TX_STACK);
    log_mic_net_stack("start");
    s_mic_tx_accumulated = 0;
    for (;;) {
        mic_net_item_t item = {};
        if (xQueueReceive(s_mic_net_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!websocket_transport_is_connected() || !audio_engine_input_session_active()) {
            s_mic_tx_accumulated = 0;
            continue;
        }
        if (item.generation != websocket_transport_generation()) {
            s_mic_tx_accumulated = 0;
            continue;
        }
        if (s_mic_tx_accumulated + MIC_FRAME_BYTES > MIC_TX_ACCUMULATOR_BYTES) {
            ESP_LOGW(TAG, "MIC TX accumulator penuh (%uB); stopping MIC session", (unsigned)s_mic_tx_accumulated);
            s_mic_tx_accumulated = 0;
            audio_engine_stop_input_session();
            continue;
        }
        memcpy(s_mic_tx_accumulator + s_mic_tx_accumulated, item.pcm, MIC_FRAME_BYTES);
        s_mic_tx_accumulated += MIC_FRAME_BYTES;
        while (s_mic_tx_accumulated >= MIC_TX_CHUNK_BYTES) {
            if (!send_frame_network(s_mic_tx_accumulator, MIC_TX_CHUNK_BYTES)) {
                s_mic_tx_accumulated = 0;
                break;
            }
            s_mic_tx_accumulated -= MIC_TX_CHUNK_BYTES;
            if (s_mic_tx_accumulated) {
                memmove(s_mic_tx_accumulator,
                        s_mic_tx_accumulator + MIC_TX_CHUNK_BYTES,
                        s_mic_tx_accumulated);
            }
        }
    }
}

static bool ensure_mic_network_task(void)
{
    if (s_mic_net_task) return true;
    s_mic_net_queue = xQueueCreateStatic(MIC_NET_QUEUE_DEPTH, sizeof(mic_net_item_t), s_mic_net_queue_storage, &s_mic_net_queue_struct);
    if (!s_mic_net_queue) return false;
    if (xTaskCreatePinnedToCore(mic_network_task, "mic_net_tx", MIC_NET_TX_STACK, nullptr,
                                MIC_NET_TX_PRIORITY, &s_mic_net_task, MIC_NET_TX_CORE) != pdPASS) {
        s_mic_net_task = nullptr; s_mic_net_queue = nullptr; return false;
    }
    return true;
}

static void mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!pcm || len != MIC_FRAME_BYTES || !websocket_transport_is_connected() ||
        !gemini_protocol_setup_complete() || !gemini_protocol_greeting_finished()) return;
    if (!ensure_mic_network_task()) { audio_engine_stop_input_session(); return; }
    mic_net_item_t item = {};
    item.generation = websocket_transport_generation();
    memcpy(item.pcm, pcm, MIC_FRAME_BYTES);
    if (xQueueSend(s_mic_net_queue, &item, 0) != pdTRUE) {
        ++s_mic_net_queue_drops;
        ESP_LOGW(TAG, "MIC network queue penuh; bounded congestion drop=%u -> stop session", (unsigned)s_mic_net_queue_drops);
        audio_engine_stop_input_session(); return;
    }
    const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(s_mic_net_queue);
    if (depth > s_mic_net_queue_high) s_mic_net_queue_high = depth;
}

void websocket_audio_init(void)
{
    (void)ensure_mic_network_task();
    if (!audio_engine_set_mic_sink(mic_sink, nullptr)) ESP_LOGE(TAG, "Gagal memasang MIC sink ke WebSocket transport");
}

bool websocket_audio_send_frame(const uint8_t *data, size_t len)
{
    if (!data || len != MIC_FRAME_BYTES || !websocket_transport_is_connected() ||
        !gemini_protocol_setup_complete() || !gemini_protocol_greeting_finished()) return false;
    mic_sink(data, len, nullptr); return true;
}

void websocket_audio_on_disconnected(void)
{
    drain_mic_net_queue();
    if (!websocket_transport_client() || !audio_engine_input_session_active()) return;
    static const char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
    (void)websocket_transport_send_text(msg, sizeof(msg) - 1U);
}
