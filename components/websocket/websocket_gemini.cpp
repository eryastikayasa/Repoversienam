#include "websocket.h"
#include "audio_engine.h"
#include "web_config.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_GEMINI";

static bool send_setup(esp_websocket_client_handle_t client)
{
    if (!client) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(generation, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));

    cJSON *speech = cJSON_AddObjectToObject(generation, "speechConfig");
    cJSON_AddStringToObject(speech, "languageCode", "id-ID");
    cJSON *voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");

    cJSON_AddStringToObject(
        setup, "model", "models/gemini-3.1-flash-live-preview");
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");

    char role[2048] = {0};
    if (web_config_load_role(role, sizeof(role)) && role[0] != '\0') {
        cJSON *instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
        cJSON *parts = cJSON_AddArrayToObject(instruction, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", role);
        cJSON_AddItemToArray(parts, part);
    }

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *vad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(vad, "disabled", false);
    cJSON_AddStringToObject(vad, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
    cJSON_AddNumberToObject(vad, "prefixPaddingMs", 40);
    cJSON_AddStringToObject(vad, "endOfSpeechSensitivity", "END_SENSITIVITY_HIGH");
    cJSON_AddNumberToObject(vad, "silenceDurationMs", 500);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    const int len = (int)strlen(json);
    const int sent = esp_websocket_client_send_text(
        client, json, len, pdMS_TO_TICKS(5000));
    free(json);

    if (sent != len) {
        ESP_LOGE(TAG, "Gemini setup gagal: sent=%d expected=%d", sent, len);
        return false;
    }

    ESP_LOGI(TAG, "Gemini setup terkirim");
    return true;
}

static bool send_audio_frame(esp_websocket_client_handle_t client,
                             const uint8_t *data, size_t len)
{
    if (!client || !data || len == 0 || len > 1600) return false;

    const size_t b64_capacity = ((len + 2U) / 3U) * 4U + 1U;
    const size_t json_capacity = b64_capacity + 160U;
    char *b64 = static_cast<char *>(malloc(b64_capacity));
    char *json = static_cast<char *>(malloc(json_capacity));
    if (!b64 || !json) {
        free(b64);
        free(json);
        return false;
    }

    size_t b64_len = 0;
    int ret = mbedtls_base64_encode(
        reinterpret_cast<unsigned char *>(b64),
        b64_capacity - 1U,
        &b64_len,
        data,
        len);
    if (ret != 0) {
        free(b64);
        free(json);
        return false;
    }
    b64[b64_len] = '\0';

    int json_len = snprintf(
        json,
        json_capacity,
        "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
        b64);

    free(b64);
    if (json_len <= 0 || (size_t)json_len >= json_capacity) {
        free(json);
        return false;
    }

    const int sent = esp_websocket_client_send_text(
        client, json, json_len, pdMS_TO_TICKS(3000));
    free(json);
    return sent == json_len;
}

static bool extract_inline_audio(const uint8_t *data, size_t len,
                                 const char **out_b64, size_t *out_len)
{
    if (!data || len == 0 || !out_b64 || !out_len) return false;
    *out_b64 = nullptr;
    *out_len = 0;

    const char *text = reinterpret_cast<const char *>(data);
    const char *inline_data = strstr(text, "\"inlineData\"");
    if (!inline_data) return false;

    const char *data_key = strstr(inline_data, "\"data\"");
    if (!data_key || data_key >= text + len) return false;

    const char *p = strchr(data_key, ':');
    if (!p || p >= text + len) return false;
    ++p;
    while (p < text + len && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= text + len || *p != '\"') return false;
    ++p;

    const char *end = p;
    while (end < text + len) {
        if (*end == '\\') return false;
        if (*end == '\"') break;
        ++end;
    }
    if (end >= text + len) return false;

    *out_b64 = p;
    *out_len = static_cast<size_t>(end - p);
    return *out_len > 0;
}

extern "C" bool websocket_gemini_on_connected(
    esp_websocket_client_handle_t client, uint32_t generation)
{
    (void)generation;
    return send_setup(client);
}

extern "C" void websocket_gemini_on_disconnected(void)
{
    ESP_LOGW(TAG, "Gemini transport disconnected");
}

extern "C" void websocket_gemini_on_data(
    const uint8_t *data, size_t len, int opcode, uint32_t generation)
{
    (void)opcode;
    if (!data || len == 0) return;

    /* Gemini audio is delivered as inlineData.data (Base64). Keep the
     * Base64 payload out of cJSON so large audio responses do not create an
     * unnecessary second decoded/parsed buffer. Audio Engine owns ingest. */
    const char *b64 = nullptr;
    size_t b64_len = 0;
    if (extract_inline_audio(data, len, &b64, &b64_len)) {
        audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_BEGIN, generation);
        if (!audio_engine_push_model_audio_base64(b64, b64_len, generation)) {
            ESP_LOGW(TAG, "AudioEngine menolak audio Gemini: %u byte Base64",
                     (unsigned)b64_len);
        }
    }

    if (len >= 18 && strstr(reinterpret_cast<const char *>(data), "\"turnComplete\":true")) {
        audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, generation);
    }
}

extern "C" bool websocket_gemini_send_audio(
    esp_websocket_client_handle_t client, const uint8_t *data, size_t len)
{
    return send_audio_frame(client, data, len);
}

extern "C" bool websocket_gemini_send_text(
    esp_websocket_client_handle_t client, const char *text)
{
    if (!client || !text) return false;
    const int len = (int)strlen(text);
    if (len <= 0) return false;
    return esp_websocket_client_send_text(
               client, text, len, pdMS_TO_TICKS(3000)) == len;
}
