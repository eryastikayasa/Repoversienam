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
static constexpr size_t RX_CAP = 32768;
static char s_rx[RX_CAP];
static size_t s_rx_len = 0;
static volatile bool s_setup_complete = false;
static volatile bool s_resume_available = false;
static char s_resume_handle[4096] = {0};
static uint64_t s_goaway_ms = 0;

static bool has(const uint8_t *d, size_t n, const char *needle)
{
    if (!d || !needle) return false;
    const size_t m = strlen(needle);
    if (!m || m > n) return false;
    for (size_t i = 0; i + m <= n; ++i) if (!memcmp(d + i, needle, m)) return true;
    return false;
}

static bool json_string(const uint8_t *d, size_t n, const char *key, char *out, size_t cap)
{
    const size_t k = strlen(key);
    if (!d || !out || cap < 2) return false;
    out[0] = 0;
    for (size_t i = 0; i + k < n; ++i) {
        if (memcmp(d + i, key, k)) continue;
        size_t p = i + k;
        while (p < n && (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n' || d[p] == ':')) ++p;
        if (p >= n || d[p] != '"') continue;
        ++p;
        size_t w = 0;
        bool esc = false;
        while (p < n) {
            const char c = (char)d[p++];
            if (esc) { if (w + 1 >= cap) return false; out[w++] = c; esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { out[w] = 0; return true; }
            if (w + 1 >= cap) return false;
            out[w++] = c;
        }
    }
    return false;
}

static void parse_goaway(const uint8_t *d, size_t n)
{
    char sec[32] = {0};
    if (!json_string(d, n, "\"seconds\"", sec, sizeof(sec))) return;
    const long long s = atoll(sec);
    if (s < 0) return;
    s_goaway_ms = (uint64_t)s * 1000ULL;
    const char key[] = "\"nanos\"";
    for (size_t i = 0; i + sizeof(key) - 1 < n; ++i) {
        if (memcmp(d + i, key, sizeof(key) - 1)) continue;
        size_t p = i + sizeof(key) - 1;
        while (p < n && (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n' || d[p] == ':')) ++p;
        long nanos = 0;
        while (p < n && d[p] >= '0' && d[p] <= '9') {
            nanos = nanos * 10 + (d[p] - '0');
            if (nanos > 999999999L) { nanos = 999999999L; break; }
            ++p;
        }
        s_goaway_ms += (uint64_t)nanos / 1000000ULL;
        break;
    }
}

static bool find_audio(const uint8_t *d, size_t n, const char **out, size_t *out_n)
{
    static const char ik[] = "\"inlineData\"";
    static const char dk[] = "\"data\"";
    *out = nullptr; *out_n = 0;
    size_t p = n;
    for (size_t i = 0; i + sizeof(ik) - 1 <= n; ++i) if (!memcmp(d + i, ik, sizeof(ik) - 1)) { p = i + sizeof(ik) - 1; break; }
    if (p == n) return false;
    size_t q = n;
    for (size_t i = p; i + sizeof(dk) - 1 <= n; ++i) {
        if (!memcmp(d + i, dk, sizeof(dk) - 1)) { q = i + sizeof(dk) - 1; break; }
        if (d[i] == '}') break;
    }
    if (q == n) return false;
    while (q < n && (d[q] == ' ' || d[q] == '\t' || d[q] == '\r' || d[q] == '\n' || d[q] == ':')) ++q;
    if (q >= n || d[q] != '"') return false;
    ++q;
    const size_t start = q;
    bool esc = false;
    for (; q < n; ++q) {
        if (esc) { esc = false; continue; }
        if (d[q] == '\\') { esc = true; continue; }
        if (d[q] == '"') { *out = (const char *)(d + start); *out_n = q - start; return *out_n != 0; }
    }
    return false;
}

static void process_json(const uint8_t *d, size_t n, uint32_t gen)
{
    if (has(d, n, "\"setupComplete\"")) {
        s_setup_complete = true;
        ESP_LOGI(TAG, "Gemini setupComplete");
    }
    if (has(d, n, "\"sessionResumptionUpdate\"")) {
        const bool resumable = has(d, n, "\"resumable\":true");
        char h[sizeof(s_resume_handle)] = {0};
        if (resumable && json_string(d, n, "\"newHandle\"", h, sizeof(h)) && h[0]) {
            strncpy(s_resume_handle, h, sizeof(s_resume_handle) - 1U);
            s_resume_available = true;
            ESP_LOGI(TAG, "Gemini session resumption handle updated");
        }
    }
    if (has(d, n, "\"goAway\"")) {
        parse_goaway(d, n);
        ESP_LOGW(TAG, "Gemini GoAway timeLeft=%llums", (unsigned long long)s_goaway_ms);
    }

    const char *b64 = nullptr; size_t b64_n = 0;
    if (find_audio(d, n, &b64, &b64_n)) {
        if (audio_engine_push_model_audio_base64(b64, b64_n, gen))
            audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_AUDIO, gen);
    }
    if (has(d, n, "\"interrupted\":true")) {
        ESP_LOGW(TAG, "Gemini interrupted -> flush playback");
        audio_engine_notify(AUDIO_ENGINE_EVENT_INTERRUPT, gen);
    }
    if (has(d, n, "\"generationComplete\":true") || has(d, n, "\"turnComplete\":true"))
        audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, gen);
}

static void reset_parser(void) { s_rx_len = 0; }

static void feed_json(const uint8_t *d, size_t n, uint32_t gen)
{
    size_t off = 0;
    while (off < n) {
        if (s_rx_len >= RX_CAP) reset_parser();
        const size_t room = RX_CAP - s_rx_len;
        const size_t take = (n - off < room) ? (n - off) : room;
        memcpy(s_rx + s_rx_len, d + off, take);
        s_rx_len += take; off += take;

        size_t depth = 0, complete = 0; bool str = false, esc = false;
        for (size_t i = 0; i < s_rx_len; ++i) {
            const char c = s_rx[i];
            if (str) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') str = false; continue; }
            if (c == '"') { str = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && depth) { if (--depth == 0) { complete = i + 1; break; } }
        }
        if (!complete) continue;
        process_json((const uint8_t *)s_rx, complete, gen);
        const size_t left = s_rx_len - complete;
        if (left) memmove(s_rx, s_rx + complete, left);
        s_rx_len = left;
    }
}

static bool send_setup(esp_websocket_client_handle_t client)
{
    char role[2048] = {0};
    const bool have_role = web_config_load_role(role, sizeof(role)) && role[0];
    char escaped[4096] = {0};
    if (have_role) {
        size_t w = 0;
        for (size_t i = 0; role[i] && w + 2 < sizeof(escaped); ++i) {
            const unsigned char c = (unsigned char)role[i];
            if (c == '"' || c == '\\') escaped[w++] = '\\';
            escaped[w++] = (c < 0x20U) ? ' ' : (char)c;
        }
        escaped[w] = 0;
    }

    char json[8192] = {0};
    const char *resume = s_resume_available ? ",\"sessionResumption\":{\"handle\":\"" : "";
    const char *resume_end = s_resume_available ? "\"}" : "";
    const char *role_part = have_role ? ",\"systemInstruction\":{\"parts\":[{\"text\":\"" : "";
    const char *role_end = have_role ? "\"}]" : "";
    const int n = snprintf(json, sizeof(json),
        "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
        "\"speechConfig\":{\"languageCode\":\"id-ID\",\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},"
        "\"contextWindowCompression\":{\"slidingWindow\":{}},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"startOfSpeechSensitivity\":\"START_SENSITIVITY_HIGH\",\"prefixPaddingMs\":40,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_HIGH\",\"silenceDurationMs\":500}}%s%s%s%s%s%s}}}",
        role_part, have_role ? escaped : "", role_end,
        resume, s_resume_available ? s_resume_handle : "", resume_end);
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;
    const int sent = esp_websocket_client_send_text(client, json, n, pdMS_TO_TICKS(2000));
    if (sent != n) return false;
    s_setup_complete = false;
    s_goaway_ms = 0;
    ESP_LOGI(TAG, "Gemini setup sent%s", s_resume_available ? " with session resume" : "");
    return true;
}

static bool send_audio_frame(esp_websocket_client_handle_t client, const uint8_t *data, size_t len)
{
    if (!client || !data || !len || len > 640) return false;
    static char b64[1024]; static char json[1200];
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64) - 1, &b64_len, data, len) != 0) return false;
    b64[b64_len] = 0;
    const int n = snprintf(json, sizeof(json), "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", b64);
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;
    return esp_websocket_client_send_text(client, json, n, pdMS_TO_TICKS(100)) == n;
}

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation) { (void)generation; reset_parser(); return send_setup(client); }
extern "C" void websocket_gemini_on_disconnected(void) { reset_parser(); s_setup_complete = false; ESP_LOGW(TAG, "Gemini disconnected"); }
extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation) { (void)opcode; feed_json(data, len, generation); }
extern "C" bool websocket_gemini_setup_complete(void) { return s_setup_complete; }
extern "C" bool websocket_gemini_should_resume(void) { return s_resume_available; }
extern "C" void websocket_gemini_clear_resume_request(void) { s_resume_available = false; }
extern "C" uint64_t websocket_gemini_goaway_time_left_ms(void) { return s_goaway_ms; }
extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len) { return send_audio_frame(client, data, len); }
extern "C" bool websocket_gemini_send_audio_stream_end(esp_websocket_client_handle_t client) { static const char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}"; return client && esp_websocket_client_send_text(client, msg, sizeof(msg)-1, pdMS_TO_TICKS(500)) == (int)(sizeof(msg)-1); }
extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text) { if (!client || !text || !*text) return false; const int n = (int)strlen(text); return esp_websocket_client_send_text(client, text, n, pdMS_TO_TICKS(1000)) == n; }
