#include "websocket.h"
#include "audio_engine.h"
#include "web_config.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_GEMINI";
static constexpr size_t GEMINI_RX_BUFFER_SIZE = 32768;
static char s_rx_buffer[GEMINI_RX_BUFFER_SIZE];
static size_t s_rx_len = 0;

static bool contains_bytes(const uint8_t *data, size_t len, const char *needle)
{
    if (!data || len == 0 || !needle || needle[0] == '\0') return false;
    const size_t nlen = strlen(needle);
    if (nlen > len) return false;
    for (size_t i = 0; i + nlen <= len; ++i) {
        if (memcmp(data + i, needle, nlen) == 0) return true;
    }
    return false;
}

static bool find_inline_audio(const uint8_t *data, size_t len, const char **out_b64, size_t *out_len)
{
    if (!data || len == 0 || !out_b64 || !out_len) return false;
    *out_b64 = nullptr; *out_len = 0;
    static const char key[] = "\"inlineData\"";
    static const char data_key[] = "\"data\"";
    const size_t key_len = sizeof(key) - 1U;
    const size_t data_key_len = sizeof(data_key) - 1U;
    size_t inline_pos = len;
    for (size_t i = 0; i + key_len <= len; ++i) {
        if (memcmp(data + i, key, key_len) == 0) { inline_pos = i + key_len; break; }
    }
    if (inline_pos == len) return false;
    size_t data_pos = len;
    for (size_t i = inline_pos; i + data_key_len <= len; ++i) {
        if (memcmp(data + i, data_key, data_key_len) == 0) { data_pos = i + data_key_len; break; }
        if (data[i] == '}') break;
    }
    if (data_pos == len) return false;
    while (data_pos < len && (data[data_pos] == ' ' || data[data_pos] == '\t' || data[data_pos] == '\r' || data[data_pos] == '\n' || data[data_pos] == ':')) ++data_pos;
    if (data_pos >= len || data[data_pos] != '"') return false;
    ++data_pos;
    const size_t start = data_pos;
    for (size_t i = start; i < len; ++i) {
        if (data[i] == '\\') return false;
        if (data[i] == '"') {
            *out_b64 = reinterpret_cast<const char *>(data + start);
            *out_len = i - start;
            return *out_len > 0;
        }
    }
    return false;
}

static void process_complete_json(const uint8_t *data, size_t len, uint32_t generation)
{
    const char *b64 = nullptr;
    size_t b64_len = 0;
    if (find_inline_audio(data, len, &b64, &b64_len)) {
        if (audio_engine_push_model_audio_base64(b64, b64_len, generation)) {
            audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_AUDIO, generation);
        } else {
            ESP_LOGW(TAG, "AudioEngine menolak audio Gemini: %u Base64", (unsigned)b64_len);
        }
    }
    if (contains_bytes(data, len, "\"turnComplete\":true")) audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, generation);
    if (contains_bytes(data, len, "\"interrupted\":true")) audio_engine_notify(AUDIO_ENGINE_EVENT_INTERRUPT, generation);
}

static void reset_rx_parser(void)
{
    s_rx_len = 0;
}

static void feed_gemini_json(const uint8_t *data, size_t len, uint32_t generation)
{
    if (!data || len == 0) return;

    size_t start = 0;
    while (start < len) {
        if (s_rx_len >= GEMINI_RX_BUFFER_SIZE) reset_rx_parser();

        const size_t room = GEMINI_RX_BUFFER_SIZE - s_rx_len;
        const size_t copy_len = (len - start) < room ? (len - start) : room;
        memcpy(s_rx_buffer + s_rx_len, data + start, copy_len);
        s_rx_len += copy_len;
        start += copy_len;

        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        size_t complete_len = 0;
        for (size_t i = 0; i < s_rx_len; ++i) {
            const char c = s_rx_buffer[i];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) {
                --depth;
                if (depth == 0) { complete_len = i + 1; break; }
            }
        }

        if (complete_len == 0) {
            if (s_rx_len == GEMINI_RX_BUFFER_SIZE) reset_rx_parser();
            continue;
        }

        process_complete_json(reinterpret_cast<const uint8_t *>(s_rx_buffer), complete_len, generation);
        const size_t remaining = s_rx_len - complete_len;
        if (remaining > 0) memmove(s_rx_buffer, s_rx_buffer + complete_len, remaining);
        s_rx_len = remaining;
    }
}

static bool send_setup(esp_websocket_client_handle_t client)
{
    if (!client) return false;
    char role[2048] = {0};
    const bool have_role = web_config_load_role(role, sizeof(role)) && role[0] != '\0';
    const size_t role_extra = have_role ? strlen(role) + 180U : 1U;
    const size_t capacity = 1400U + role_extra;
    char *json = static_cast<char *>(malloc(capacity));
    if (!json) return false;

    int n;
    if (have_role) {
        size_t escaped_len = 0;
        for (size_t i = 0; role[i] != '\0'; ++i) {
            const unsigned char c = (unsigned char)role[i];
            escaped_len += (c == '"' || c == '\\' || c < 0x20U) ? 2U : 1U;
        }
        char *escaped = static_cast<char *>(malloc(escaped_len + 1U));
        if (!escaped) { free(json); return false; }
        size_t p = 0;
        for (size_t i = 0; role[i] != '\0'; ++i) {
            const unsigned char c = (unsigned char)role[i];
            if (c == '"') { escaped[p++] = '\\'; escaped[p++] = '"'; }
            else if (c == '\\') { escaped[p++] = '\\'; escaped[p++] = '\\'; }
            else if (c < 0x20U) escaped[p++] = ' ';
            else escaped[p++] = (char)c;
        }
        escaped[p] = '\0';
        n = snprintf(json, capacity,
            "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"languageCode\":\"id-ID\",\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},\"inputAudioTranscription\":{},\"systemInstruction\":{\"parts\":[{\"text\":\"%s\"}]},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"startOfSpeechSensitivity\":\"START_SENSITIVITY_HIGH\",\"prefixPaddingMs\":40,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_HIGH\",\"silenceDurationMs\":500}}}}}", escaped);
        free(escaped);
    } else {
        n = snprintf(json, capacity,
            "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"languageCode\":\"id-ID\",\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},\"inputAudioTranscription\":{},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"startOfSpeechSensitivity\":\"START_SENSITIVITY_HIGH\",\"prefixPaddingMs\":40,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_HIGH\",\"silenceDurationMs\":500}}}}}");
    }
    if (n <= 0 || (size_t)n >= capacity) { free(json); return false; }
    const int sent = esp_websocket_client_send_text(client, json, n, pdMS_TO_TICKS(5000));
    free(json);
    if (sent != n) { ESP_LOGE(TAG, "Gemini setup gagal: sent=%d expected=%d", sent, n); return false; }
    ESP_LOGI(TAG, "Gemini setup terkirim");
    return true;
}

static bool send_audio_frame(esp_websocket_client_handle_t client, const uint8_t *data, size_t len)
{
    if (!client || !data || len == 0 || len > 1600U) return false;
    const size_t b64_capacity = ((len + 2U) / 3U) * 4U + 1U;
    const size_t json_capacity = b64_capacity + 180U;
    char *b64 = static_cast<char *>(malloc(b64_capacity));
    char *json = static_cast<char *>(malloc(json_capacity));
    if (!b64 || !json) { free(b64); free(json); return false; }
    size_t b64_len = 0;
    const int ret = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64), b64_capacity - 1U, &b64_len, data, len);
    if (ret != 0) { free(b64); free(json); return false; }
    b64[b64_len] = '\0';
    const int json_len = snprintf(json, json_capacity, "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", b64);
    free(b64);
    if (json_len <= 0 || (size_t)json_len >= json_capacity) { free(json); return false; }
    const int sent = esp_websocket_client_send_text(client, json, json_len, pdMS_TO_TICKS(3000));
    free(json);
    return sent == json_len;
}

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation)
{
    (void)generation;
    reset_rx_parser();
    return send_setup(client);
}

extern "C" void websocket_gemini_on_disconnected(void)
{
    reset_rx_parser();
    ESP_LOGW(TAG, "Gemini transport disconnected");
}

extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation)
{
    (void)opcode;
    feed_gemini_json(data, len, generation);
}

extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len)
{
    return send_audio_frame(client, data, len);
}

extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text)
{
    if (!client || !text) return false;
    const int len = (int)strlen(text);
    if (len <= 0) return false;
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(3000)) == len;
}
