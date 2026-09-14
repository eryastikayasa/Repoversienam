#include "websocket_audio.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";
static char s_audio_b64[1024];
static char s_audio_json[1200];

static uint64_t s_profile_last_us = 0;
static uint64_t s_profile_count = 0;
static uint64_t s_profile_encode_us = 0;
static uint64_t s_profile_encode_max_us = 0;
static uint64_t s_profile_json_us = 0;
static uint64_t s_profile_json_max_us = 0;
static uint64_t s_profile_poll_us = 0;
static uint64_t s_profile_poll_max_us = 0;
static uint64_t s_profile_tls_us = 0;
static uint64_t s_profile_tls_max_us = 0;
static uint64_t s_profile_transport_us = 0;
static uint64_t s_profile_transport_max_us = 0;
static uint64_t s_profile_send_us = 0;
static uint64_t s_profile_send_max_us = 0;
static uint64_t s_profile_total_us = 0;
static uint64_t s_profile_total_max_us = 0;

static void profile_record(uint32_t encode_us, uint32_t json_us,
                           uint32_t poll_us, uint32_t tls_us,
                           uint32_t transport_us, uint32_t send_us,
                           uint32_t total_us)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    ++s_profile_count;
    s_profile_encode_us += encode_us;
    s_profile_json_us += json_us;
    s_profile_poll_us += poll_us;
    s_profile_tls_us += tls_us;
    s_profile_transport_us += transport_us;
    s_profile_send_us += send_us;
    s_profile_total_us += total_us;
    if (encode_us > s_profile_encode_max_us) s_profile_encode_max_us = encode_us;
    if (json_us > s_profile_json_max_us) s_profile_json_max_us = json_us;
    if (poll_us > s_profile_poll_max_us) s_profile_poll_max_us = poll_us;
    if (tls_us > s_profile_tls_max_us) s_profile_tls_max_us = tls_us;
    if (transport_us > s_profile_transport_max_us) s_profile_transport_max_us = transport_us;
    if (send_us > s_profile_send_max_us) s_profile_send_max_us = send_us;
    if (total_us > s_profile_total_max_us) s_profile_total_max_us = total_us;
    if (!s_profile_last_us) s_profile_last_us = now_us;

    if (now_us - s_profile_last_us >= 5000000ULL) {
        ESP_LOGI(TAG,
                 "MIC_TX_PROFILE: count=%llu encode_avg_us=%llu encode_max_us=%llu json_avg_us=%llu json_max_us=%llu poll_write_avg_us=%llu poll_write_max_us=%llu tls_write_avg_us=%llu tls_write_max_us=%llu transport_write_avg_us=%llu transport_write_max_us=%llu send_avg_us=%llu send_max_us=%llu total_avg_us=%llu total_max_us=%llu",
                 (unsigned long long)s_profile_count,
                 (unsigned long long)(s_profile_encode_us / s_profile_count),
                 (unsigned long long)s_profile_encode_max_us,
                 (unsigned long long)(s_profile_json_us / s_profile_count),
                 (unsigned long long)s_profile_json_max_us,
                 (unsigned long long)(s_profile_poll_us / s_profile_count),
                 (unsigned long long)s_profile_poll_max_us,
                 (unsigned long long)(s_profile_tls_us / s_profile_count),
                 (unsigned long long)s_profile_tls_max_us,
                 (unsigned long long)(s_profile_transport_us / s_profile_count),
                 (unsigned long long)s_profile_transport_max_us,
                 (unsigned long long)(s_profile_send_us / s_profile_count),
                 (unsigned long long)s_profile_send_max_us,
                 (unsigned long long)(s_profile_total_us / s_profile_count),
                 (unsigned long long)s_profile_total_max_us);
        s_profile_last_us = now_us;
        s_profile_count = 0;
        s_profile_encode_us = 0;
        s_profile_encode_max_us = 0;
        s_profile_json_us = 0;
        s_profile_json_max_us = 0;
        s_profile_poll_us = 0;
        s_profile_poll_max_us = 0;
        s_profile_tls_us = 0;
        s_profile_tls_max_us = 0;
        s_profile_transport_us = 0;
        s_profile_transport_max_us = 0;
        s_profile_send_us = 0;
        s_profile_send_max_us = 0;
        s_profile_total_us = 0;
        s_profile_total_max_us = 0;
    }
}

static void mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!pcm || len != 640U || !websocket_transport_is_connected() ||
        !gemini_protocol_setup_complete() || !gemini_protocol_greeting_finished()) {
        return;
    }
    (void)websocket_audio_send_frame(pcm, len);
}

void websocket_audio_init(void)
{
    if (!audio_engine_set_mic_sink(mic_sink, nullptr)) {
        ESP_LOGE(TAG, "Gagal memasang MIC sink ke WebSocket transport");
    }
}

bool websocket_audio_send_frame(const uint8_t *data, size_t len)
{
    if (!data || len != 640U || !websocket_transport_is_connected() ||
        !gemini_protocol_setup_complete() || !gemini_protocol_greeting_finished()) {
        return false;
    }

    const int64_t total_start_us = esp_timer_get_time();
    const int64_t encode_start_us = total_start_us;
    size_t b64_len = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char *>(s_audio_b64), sizeof(s_audio_b64) - 1,
            &b64_len, data, len) != 0) {
        return false;
    }
    s_audio_b64[b64_len] = '\0';
    const uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_start_us);

    const int64_t json_start_us = esp_timer_get_time();
    const int n = snprintf(
        s_audio_json, sizeof(s_audio_json),
        "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
        s_audio_b64);
    const uint32_t json_us = (uint32_t)(esp_timer_get_time() - json_start_us);
    if (n <= 0 || (size_t)n >= sizeof(s_audio_json)) return false;

    uint64_t poll_before = 0;
    uint64_t tls_before = 0;
    uint64_t transport_before = 0;
    int poll_ret_before = 0;
    int transport_ret_before = 0;
    ssize_t tls_ret_before = 0;
    websocket_transport_profile_snapshot(&poll_before, &tls_before, &transport_before,
                                         &poll_ret_before, &transport_ret_before, &tls_ret_before);

    const int64_t send_start_us = esp_timer_get_time();
    const bool sent = websocket_transport_send_text(s_audio_json, (size_t)n) == ESP_OK;
    const uint32_t send_us = (uint32_t)(esp_timer_get_time() - send_start_us);

    uint64_t poll_after = 0;
    uint64_t tls_after = 0;
    uint64_t transport_after = 0;
    int poll_ret_after = 0;
    int transport_ret_after = 0;
    ssize_t tls_ret_after = 0;
    websocket_transport_profile_snapshot(&poll_after, &tls_after, &transport_after,
                                         &poll_ret_after, &transport_ret_after, &tls_ret_after);

    const uint32_t poll_us = (uint32_t)(poll_after - poll_before);
    const uint32_t tls_us = (uint32_t)(tls_after - tls_before);
    const uint32_t transport_us = (uint32_t)(transport_after - transport_before);
    const uint32_t total_us = (uint32_t)(esp_timer_get_time() - total_start_us);
    profile_record(encode_us, json_us, poll_us, tls_us, transport_us, send_us, total_us);
    return sent;
}

void websocket_audio_on_disconnected(void)
{
    if (!websocket_transport_client()) return;
    if (!audio_engine_input_session_active()) return;
    static const char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
    (void)websocket_transport_send_text(msg, sizeof(msg) - 1U);
}
