#include "websocket.h"
#include "audio_engine.h"
#include "web_config.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_GEMINI";
static volatile bool s_setup_complete = false;
static volatile bool s_greeting_sent = false;
static volatile bool s_greeting_finished = false;
static volatile bool s_resume_available = false;
static EXT_RAM_BSS_ATTR char s_resume_handle[4096] = {0};
static uint64_t s_goaway_ms = 0;
static esp_websocket_client_handle_t s_client = nullptr;

static EXT_RAM_BSS_ATTR char s_setup_role[2048] = {0};
static EXT_RAM_BSS_ATTR char s_setup_escaped[4096] = {0};
static EXT_RAM_BSS_ATTR char s_setup_json[8192] = {0};
static EXT_RAM_BSS_ATTR char s_audio_b64[1024];
static EXT_RAM_BSS_ATTR char s_audio_json[1200];

extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text);

/* Repo5-style message classification: complete WebSocket JSON arrives here,
 * then protocol state and server audio are processed separately. */
typedef enum {
    GEMINI_MESSAGE_UNKNOWN = 0,
    GEMINI_MESSAGE_SETUP,
    GEMINI_MESSAGE_SERVER_CONTENT,
    GEMINI_MESSAGE_SESSION_RESUMPTION,
    GEMINI_MESSAGE_GOAWAY,
    GEMINI_MESSAGE_ERROR
} gemini_message_type_t;

static gemini_message_type_t gemini_message_classify(cJSON *root)
{
    if (!cJSON_IsObject(root)) return GEMINI_MESSAGE_UNKNOWN;
    if (cJSON_GetObjectItemCaseSensitive(root, "error")) return GEMINI_MESSAGE_ERROR;
    if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete")) return GEMINI_MESSAGE_SETUP;
    if (cJSON_GetObjectItemCaseSensitive(root, "serverContent")) return GEMINI_MESSAGE_SERVER_CONTENT;
    if (cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate")) return GEMINI_MESSAGE_SESSION_RESUMPTION;
    if (cJSON_GetObjectItemCaseSensitive(root, "goAway")) return GEMINI_MESSAGE_GOAWAY;
    return GEMINI_MESSAGE_UNKNOWN;
}

static bool json_string(cJSON *object, const char *key, char *out, size_t cap)
{
    if (!cJSON_IsObject(object) || !key || !out || cap < 2) return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t len = strlen(item->valuestring);
    if (len + 1U > cap) return false;
    memcpy(out, item->valuestring, len + 1U);
    return true;
}

static void parse_goaway(cJSON *root)
{
    cJSON *goaway = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    if (!cJSON_IsObject(goaway)) return;
    cJSON *seconds = cJSON_GetObjectItemCaseSensitive(goaway, "timeLeft");
    if (!cJSON_IsObject(seconds)) return;
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(seconds, "seconds");
    cJSON *nanos = cJSON_GetObjectItemCaseSensitive(seconds, "nanos");
    if (cJSON_IsNumber(sec)) s_goaway_ms = (uint64_t)(sec->valuedouble * 1000.0);
    if (cJSON_IsNumber(nanos) && nanos->valuedouble > 0.0)
        s_goaway_ms += (uint64_t)(nanos->valuedouble / 1000000.0);
}

static bool send_greeting_text(void)
{
    if (!s_client || !s_setup_complete || s_greeting_sent) return false;
    static const char msg[] =
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":\""
        "Mulai percakapan dengan mengucapkan tepat: Halo, ada yang bisa dibantu?"
        "\"}]}],\"turnComplete\":true}}";
    if (!websocket_gemini_send_text(s_client, msg)) return false;
    s_greeting_sent = true;
    s_greeting_finished = false;
    ESP_LOGI(TAG, "WS_GEMINI: Greeting JSON sent");
    return true;
}

/* Gemini serverContent -> modelTurn -> parts[] -> inlineData -> base64 PCM16.
 * This function owns Gemini audio parsing only; AudioEngine remains the audio owner. */
static bool gemini_audio_process_server_message(cJSON *root, uint32_t generation)
{
    if (!cJSON_IsObject(root)) return false;
    cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (!cJSON_IsObject(server)) return false;

    bool handled = false;
    cJSON *interrupted = cJSON_GetObjectItemCaseSensitive(server, "interrupted");
    if (cJSON_IsTrue(interrupted)) {
        ESP_LOGW(TAG, "Gemini interrupted -> flush playback");
        audio_engine_notify(AUDIO_ENGINE_EVENT_INTERRUPT, generation);
        handled = true;
    }

    cJSON *model_turn = cJSON_GetObjectItemCaseSensitive(server, "modelTurn");
    cJSON *parts = model_turn ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
    if (cJSON_IsArray(parts)) {
        const int count = cJSON_GetArraySize(parts);
        for (int i = 0; i < count; ++i) {
            cJSON *part = cJSON_GetArrayItem(parts, i);
            if (!cJSON_IsObject(part)) continue;
            cJSON *inline_data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
            if (!cJSON_IsObject(inline_data)) continue;

            cJSON *mime = cJSON_GetObjectItemCaseSensitive(inline_data, "mimeType");
            cJSON *encoded = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
            if (!cJSON_IsString(encoded) || !encoded->valuestring || !encoded->valuestring[0]) continue;
            if (cJSON_IsString(mime) && mime->valuestring &&
                strncmp(mime->valuestring, "audio/pcm", strlen("audio/pcm")) != 0) {
                ESP_LOGW(TAG, "Gemini inlineData mimeType bukan PCM: %s", mime->valuestring);
                continue;
            }

            const size_t b64_len = strlen(encoded->valuestring);
            const size_t capacity = (b64_len / 4U) * 3U + 3U;
            uint8_t *pcm = static_cast<uint8_t *>(malloc(capacity));
            if (!pcm) {
                ESP_LOGE(TAG, "Gemini audio decode buffer gagal: %u byte", (unsigned)capacity);
                continue;
            }

            size_t decoded_len = 0;
            const int rc = mbedtls_base64_decode(
                pcm, capacity, &decoded_len,
                reinterpret_cast<const unsigned char *>(encoded->valuestring), b64_len);
            if (rc != 0 || decoded_len == 0 || (decoded_len & 1U) != 0) {
                ESP_LOGW(TAG, "Gemini audio Base64 gagal: rc=%d b64=%u decoded=%u",
                         rc, (unsigned)b64_len, (unsigned)decoded_len);
                free(pcm);
                continue;
            }

            ESP_LOGI(TAG, "GEMINI_AUDIO: inlineData PCM16 b64=%u decoded=%u",
                     (unsigned)b64_len, (unsigned)decoded_len);
            if (!audio_engine_push_model_audio(pcm, decoded_len, generation)) {
                ESP_LOGW(TAG, "GEMINI_AUDIO: PCM gagal dikirim ke AudioEngine: %u byte",
                         (unsigned)decoded_len);
            } else {
                ESP_LOGI(TAG, "GEMINI_AUDIO: PCM dikirim ke AudioEngine: %u byte",
                         (unsigned)decoded_len);
                audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_AUDIO, generation);
                handled = true;
            }
            free(pcm);
        }
    }

    cJSON *turn_complete = cJSON_GetObjectItemCaseSensitive(server, "turnComplete");
    cJSON *generation_complete = cJSON_GetObjectItemCaseSensitive(server, "generationComplete");
    if (cJSON_IsTrue(turn_complete) || cJSON_IsTrue(generation_complete)) {
        audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, generation);
        handled = true;
    }
    return handled;
}

static void gemini_protocol_process_message(cJSON *root, const char *json, size_t len, uint32_t generation)
{
    const gemini_message_type_t type = gemini_message_classify(root);
    switch (type) {
    case GEMINI_MESSAGE_SETUP:
        if (!s_setup_complete) {
            s_setup_complete = true;
            ESP_LOGI(TAG, "WS_GEMINI: Gemini setupComplete");
            if (!send_greeting_text()) ESP_LOGW(TAG, "WS_GEMINI: Greeting JSON gagal dikirim");
        }
        break;

    case GEMINI_MESSAGE_SERVER_CONTENT:
        (void)gemini_audio_process_server_message(root, generation);
        break;

    case GEMINI_MESSAGE_SESSION_RESUMPTION: {
        cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
        cJSON *resumable = cJSON_GetObjectItemCaseSensitive(update, "resumable");
        if (cJSON_IsTrue(resumable) && json_string(update, "newHandle", s_resume_handle, sizeof(s_resume_handle))) {
            if (s_resume_handle[0]) {
                s_resume_available = true;
                ESP_LOGI(TAG, "Gemini session resumption handle updated");
            }
        }
        break;
    }

    case GEMINI_MESSAGE_GOAWAY:
        parse_goaway(root);
        ESP_LOGW(TAG, "Gemini GoAway timeLeft=%llums", (unsigned long long)s_goaway_ms);
        break;

    case GEMINI_MESSAGE_ERROR:
        ESP_LOGE(TAG, "WS_GEMINI: SERVER ERROR");
        ESP_LOGE(TAG, "WS_GEMINI: SERVER ERROR RAW: %.*s", (int)(len < 512U ? len : 512U), json);
        audio_engine_notify(AUDIO_ENGINE_EVENT_ERROR, generation);
        break;

    default:
        ESP_LOGW(TAG, "WS_RX: Gemini RX belum dipetakan");
        ESP_LOGW(TAG, "WS_RX: Gemini RX RAW: %.*s", (int)(len < 512U ? len : 512U), json);
        break;
    }

    if (s_greeting_sent && !s_greeting_finished) {
        cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
        const bool done = cJSON_IsObject(server) &&
            (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "generationComplete")) ||
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "turnComplete")));
        if (done) {
            s_greeting_finished = true;
            ESP_LOGI(TAG, "WS_GEMINI: Greeting selesai");
            ESP_LOGI(TAG, "WEBSOCKET: Greeting selesai -> MIC streaming ENABLED");
        }
    }
}

static void process_complete_json(const uint8_t *data, size_t len, uint32_t generation)
{
    if (!data || len == 0) return;
    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), len);
    if (!root) {
        ESP_LOGW(TAG, "WS_RX: Gemini RX JSON invalid len=%u", (unsigned)len);
        ESP_LOGW(TAG, "WS_RX: Gemini RX RAW: %.*s", (int)(len < 512U ? len : 512U), data);
        return;
    }
    gemini_protocol_process_message(root, reinterpret_cast<const char *>(data), len, generation);
    cJSON_Delete(root);
}

static bool send_setup(esp_websocket_client_handle_t client)
{
    s_setup_role[0] = 0;
    const bool have_role = web_config_load_role(s_setup_role, sizeof(s_setup_role)) && s_setup_role[0];
    s_setup_escaped[0] = 0;
    if (have_role) {
        size_t w = 0;
        for (size_t i = 0; s_setup_role[i] && w + 2 < sizeof(s_setup_escaped); ++i) {
            const unsigned char c = (unsigned char)s_setup_role[i];
            if (c == '"' || c == '\\') s_setup_escaped[w++] = '\\';
            s_setup_escaped[w++] = (c < 0x20U) ? ' ' : (char)c;
        }
        s_setup_escaped[w] = 0;
    }
    s_setup_json[0] = 0;
    const char *resume = s_resume_available ? ",\"sessionResumption\":{\"handle\":\"" : ",\"sessionResumption\":{}";
    const char *resume_end = s_resume_available ? "\"}" : "";
    const char *role_part = have_role ? ",\"systemInstruction\":{\"parts\":[{\"text\":\"" : "";
    const char *role_end = have_role ? "\"}]}" : "";
    const int n = snprintf(s_setup_json, sizeof(s_setup_json),
        "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
        "\"speechConfig\":{\"languageCode\":\"id-ID\",\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},"
        "\"contextWindowCompression\":{\"slidingWindow\":{}},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"startOfSpeechSensitivity\":\"START_SENSITIVITY_HIGH\",\"prefixPaddingMs\":40,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_HIGH\",\"silenceDurationMs\":500}}%s%s%s%s%s%s}}}",
        role_part, have_role ? s_setup_escaped : "", role_end,
        resume, s_resume_available ? s_resume_handle : "", resume_end);
    if (n <= 0 || (size_t)n >= sizeof(s_setup_json)) return false;
    const int sent = esp_websocket_client_send_text(client, s_setup_json, n, pdMS_TO_TICKS(2000));
    if (sent != n) return false;
    s_setup_complete = false;
    s_greeting_sent = false;
    s_greeting_finished = false;
    s_goaway_ms = 0;
    s_client = client;
    ESP_LOGI(TAG, "Gemini setup sent%s", s_resume_available ? " with session resume" : "");
    return true;
}

static bool send_audio_frame(esp_websocket_client_handle_t client, const uint8_t *data, size_t len)
{
    if (!client || !data || !len || len > 640) return false;
    size_t b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)s_audio_b64, sizeof(s_audio_b64) - 1, &b64_len, data, len) != 0) return false;
    s_audio_b64[b64_len] = 0;
    const int n = snprintf(s_audio_json, sizeof(s_audio_json), "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", s_audio_b64);
    if (n <= 0 || (size_t)n >= sizeof(s_audio_json)) return false;
    return esp_websocket_client_send_text(client, s_audio_json, n, pdMS_TO_TICKS(100)) == n;
}

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation)
{
    (void)generation;
    s_client = client;
    return send_setup(client);
}

extern "C" void websocket_gemini_on_disconnected(void)
{
    s_setup_complete = false;
    s_greeting_sent = false;
    s_greeting_finished = false;
    s_client = nullptr;
    ESP_LOGW(TAG, "Gemini disconnected");
}

extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation)
{
    (void)opcode;
    /* Called only by the WebSocket RX worker after complete-message assembly. */
    process_complete_json(data, len, generation);
}

extern "C" bool websocket_gemini_setup_complete(void) { return s_setup_complete; }
extern "C" bool websocket_gemini_greeting_finished(void) { return s_greeting_finished; }
extern "C" bool websocket_gemini_should_resume(void) { return s_resume_available; }
extern "C" uint64_t websocket_gemini_goaway_time_left_ms(void) { return s_goaway_ms; }
extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len) { return send_audio_frame(client, data, len); }
extern "C" bool websocket_gemini_send_audio_stream_end(esp_websocket_client_handle_t client)
{
    static const char msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
    return client && esp_websocket_client_send_text(client, msg, sizeof(msg)-1, pdMS_TO_TICKS(500)) == (int)(sizeof(msg)-1);
}
extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text)
{
    if (!client || !text || !*text) return false;
    const int n = (int)strlen(text);
    return esp_websocket_client_send_text(client, text, n, pdMS_TO_TICKS(1000)) == n;
}
