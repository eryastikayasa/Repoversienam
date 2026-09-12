#include "websocket_audio.h"
#include "websocket_transport.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";
static char s_audio_b64[1024];
static char s_audio_json[1200];

static void mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    /* This is the existing Repo6 AudioEngine frame boundary. It is not an
     * audio queue or buffer owned by WebSocket. */
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

    size_t b64_len = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char *>(s_audio_b64), sizeof(s_audio_b64) - 1,
            &b64_len, data, len) != 0) {
        return false;
    }
    s_audio_b64[b64_len] = '\0';

    const int n = snprintf(
        s_audio_json, sizeof(s_audio_json),
        "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
        s_audio_b64);
    if (n <= 0 || (size_t)n >= sizeof(s_audio_json)) return false;

    return websocket_transport_send_text(s_audio_json, (size_t)n) == ESP_OK;
}

void websocket_audio_on_disconnected(void)
{
    if (!websocket_transport_client()) return;
    if (!audio_engine_input_session_active()) return;
    static const char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
    (void)websocket_transport_send_text(msg, sizeof(msg) - 1U);
}
