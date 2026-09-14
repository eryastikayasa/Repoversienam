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
static constexpr size_t MIC_NET_QUEUE_DEPTH = 4U;
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
static char s_audio_b64[1024];
static char s_audio_json[1200];
static uint64_t s_profile_last_us = 0, s_profile_count = 0;
static uint64_t s_profile_encode_us = 0, s_profile_encode_max_us = 0;
static uint64_t s_profile_json_us = 0, s_profile_json_max_us = 0;
static uint64_t s_profile_poll_us = 0, s_profile_poll_max_us = 0;
static uint64_t s_profile_tls_us = 0, s_profile_tls_max_us = 0;
static uint64_t s_profile_transport_us = 0, s_profile_transport_max_us = 0;
static uint64_t s_profile_send_us = 0, s_profile_send_max_us = 0;
static uint64_t s_profile_total_us = 0, s_profile_total_max_us = 0;

static void drain_mic_net_queue(void)
{
    if (!s_mic_net_queue) return;
    mic_net_item_t item = {};
    while (xQueueReceive(s_mic_net_queue, &item, 0) == pdTRUE) {}
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
    if (now_us - s_profile_last_us < 5000000ULL || s_profile_count == 0) return;
    const uint64_t count = s_profile_count;
    ESP_LOGI(TAG,
        "MIC_TX_PROFILE: count=%llu encode_avg_us=%llu encode_max_us=%llu json_avg_us=%llu json_max_us=%llu poll_write_avg_us=%llu poll_write_max_us=%llu tls_write_avg_us=%llu tls_write_max_us=%llu transport_write_avg_us=%llu transport_write_max_us=%llu send_avg_us=%llu send_max_us=%llu total_avg_us=%llu total_max_us=%llu net_queue_high=%u net_queue_drops=%u net_stack_watermark=%uB",
        (unsigned long long)count,
        (unsigned long long)(s_profile_encode_us / count), (unsigned long long)s_profile_encode_max_us,
        (unsigned long long)(s_profile_json_us / count), (unsigned long long)s_profile_json_max_us,
        (unsigned long long)(s_profile_poll_us / count), (unsigned long long)s_profile_poll_max_us,
        (unsigned long long)(s_profile_tls_us / count), (unsigned long long)s_profile_tls_max_us,
        (unsigned long long)(s_profile_transport_us / count), (unsigned long long)s_profile_transport_max_us,
        (unsigned long long)(s_profile_send_us / count), (unsigned long long)s_profile_send_max_us,
        (unsigned long long)(s_profile_total_us / count), (unsigned long long)s_profile_total_max_us,
        (unsigned)s_mic_net_queue_high, (unsigned)s_mic_net_queue_drops,
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
    log_mic_net_stack("profile");
}
static bool send_frame_network(const uint8_t *data, size_t len)
{
    if (!data || len != MIC_FRAME_BYTES || !websocket_transport_is_connected() ||
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
    websocket_transport_profile_snapshot(&poll_before, &tls_before, &transport_before);
    const int64_t send_start = esp_timer_get_time();
    const bool sent = websocket_transport_send_text(s_audio_json, (size_t)n) == ESP_OK;
    const uint32_t send_us = (uint32_t)(esp_timer_get_time() - send_start);
    uint64_t poll_after = 0, tls_after = 0, transport_after = 0;
    websocket_transport_profile_snapshot(&poll_after, &tls_after, &transport_after);
    const uint32_t total_us = (uint32_t)(esp_timer_get_time() - total_start);
    profile_record(encode_us, json_us, (uint32_t)(poll_after - poll_before),
                   (uint32_t)(tls_after - tls_before), (uint32_t)(transport_after - transport_before),
                   send_us, total_us);
    if (!sent) { ESP_LOGW(TAG, "MIC_NET_TX: bounded send failed/timeout; stopping MIC input session"); audio_engine_stop_input_session(); }
    return sent;
}
static void mic_network_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MIC network sender START queue=%u frame=%uB stack=%uB timeout=20ms",
             (unsigned)MIC_NET_QUEUE_DEPTH, (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_NET_TX_STACK);
    log_mic_net_stack("start");
    for (;;) {
        mic_net_item_t item = {};
        if (xQueueReceive(s_mic_net_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!websocket_transport_is_connected() || !audio_engine_input_session_active()) continue;
        if (item.generation != websocket_transport_generation()) continue;
        (void)send_frame_network(item.pcm, MIC_FRAME_BYTES);
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
