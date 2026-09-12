#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "websocket_transport.h"
#include "web_config.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "GEMINI_PROTO";
static volatile bool s_setup_complete = false;
static volatile bool s_greeting_sent = false;
static volatile bool s_greeting_finished = false;
static volatile bool s_resume_available = false;
static bool s_resume_attempted = false;
static char s_resume_handle[4096] = {0};
static char s_setup_role[2048] = {0};
static char s_setup_escaped[4096] = {0};
static char s_setup_json[8192] = {0};
static uint64_t s_goaway_ms = 0;

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
    cJSON *time_left = cJSON_GetObjectItemCaseSensitive(goaway, "timeLeft");
    if (!cJSON_IsObject(time_left)) return;
    cJSON *seconds = cJSON_GetObjectItemCaseSensitive(time_left, "seconds");
    cJSON *nanos = cJSON_GetObjectItemCaseSensitive(time_left, "nanos");
    s_goaway_ms = 0;
    if (cJSON_IsNumber(seconds)) s_goaway_ms += (uint64_t)(seconds->valuedouble * 1000.0);
    if (cJSON_IsNumber(nanos) && nanos->valuedouble > 0.0)
        s_goaway_ms += (uint64_t)(nanos->valuedouble / 1000000.0);
}

static bool send_greeting(void)
{
    if (!s_setup_complete || s_greeting_sent || !websocket_transport_is_connected()) return false;
    static const char greeting[] =
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":\""
        "Mulai percakapan dengan mengucapkan tepat: Halo, ada yang bisa dibantu?"
        "\"}]}],\"turnComplete\":true}}";
    if (websocket_transport_send_text(greeting, sizeof(greeting) - 1U) != ESP_OK) return false;
    s_greeting_sent = true;
    s_greeting_finished = false;
    ESP_LOGI(TAG, "WS_GEMINI: Greeting JSON sent");
    return true;
}

static bool build_setup(void)
{
    s_setup_role[0] = '\0';
    const bool have_role = web_config_load_role(s_setup_role, sizeof(s_setup_role)) && s_setup_role[0];

    s_setup_escaped[0] = '\0';
    if (have_role) {
        size_t w = 0;
        for (size_t i = 0; s_setup_role[i] && w + 2U < sizeof(s_setup_escaped); ++i) {
            const unsigned char c = (unsigned char)s_setup_role[i];
            if (c == '"' || c == '\\') s_setup_escaped[w++] = '\\';
            s_setup_escaped[w++] = (c < 0x20U) ? ' ' : (char)c;
        }
        s_setup_escaped[w] = '\0';
    }

    const char *resume = s_resume_available
        ? ",\"sessionResumption\":{\"handle\":\""
        : ",\"sessionResumption\":{}";
    const char *resume_end = s_resume_available ? "\"}" : "";
    const char *role_part = have_role
        ? ",\"systemInstruction\":{\"parts\":[{\"text\":\""
        : "";
    const char *role_end = have_role ? "\"}]}" : "";

    const int n = snprintf(
        s_setup_json, sizeof(s_setup_json),
        "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
        "\"speechConfig\":{\"languageCode\":\"id-ID\",\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}},"
        "\"contextWindowCompression\":{\"slidingWindow\":{}},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"startOfSpeechSensitivity\":\"START_SENSITIVITY_HIGH\",\"prefixPaddingMs\":40,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_HIGH\",\"silenceDurationMs\":500}}%s%s%s%s%s%s}}}",
        role_part, have_role ? s_setup_escaped : "", role_end,
        resume, s_resume_available ? s_resume_handle : "", resume_end);
    return n > 0 && (size_t)n < sizeof(s_setup_json);
}

bool gemini_protocol_on_connected(void)
{
    s_setup_complete = false;
    s_greeting_sent = false;
    s_greeting_finished = false;
    s_resume_attempted = false;
    s_goaway_ms = 0;

    if (!build_setup()) {
        ESP_LOGE(TAG, "Gemini setup JSON gagal dibuat");
        return false;
    }
    const size_t len = strlen(s_setup_json);
    if (websocket_transport_send_text(s_setup_json, len) != ESP_OK) {
        ESP_LOGE(TAG, "Gemini setup gagal dikirim");
        return false;
    }
    ESP_LOGI(TAG, "Gemini setup sent");
    ESP_LOGI(TAG, "Waiting for Gemini setupComplete before greeting");
    return true;
}

void gemini_protocol_on_disconnected(void)
{
    s_setup_complete = false;
    s_greeting_sent = false;
    s_greeting_finished = false;
    s_goaway_ms = 0;
    s_resume_attempted = false;
}

bool gemini_protocol_process_message(const char *json, size_t len, uint32_t generation)
{
    if (!json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Gemini RX JSON invalid len=%u", (unsigned)len);
        ESP_LOGW(TAG, "Gemini RX first bytes: %02X %02X %02X %02X",
                 len > 0 ? (unsigned char)json[0] : 0,
                 len > 1 ? (unsigned char)json[1] : 0,
                 len > 2 ? (unsigned char)json[2] : 0,
                 len > 3 ? (unsigned char)json[3] : 0);
        const size_t tail = len < 16U ? 0U : len - 16U;
        ESP_LOGW(TAG, "Gemini RX last bytes: %02X %02X %02X %02X",
                 len > tail ? (unsigned char)json[tail] : 0,
                 len > tail + 1U ? (unsigned char)json[tail + 1U] : 0,
                 len > tail + 2U ? (unsigned char)json[tail + 2U] : 0,
                 len > tail + 3U ? (unsigned char)json[tail + 3U] : 0);
        return false;
    }

    const gemini_message_type_t type = gemini_message_classify(json, len);
    bool handled = true;

    switch (type) {
    case GEMINI_MESSAGE_SETUP:
        if (!s_setup_complete) {
            s_setup_complete = true;
            ESP_LOGI(TAG, "GEMINI_PROTO: Gemini setupComplete");
            ESP_LOGI("WEBSOCKET", "Gemini setupComplete - audio uplink READY");
            if (!send_greeting()) {
                ESP_LOGW(TAG, "WS_GEMINI: Greeting JSON gagal dikirim");
            }
        }
        break;

    case GEMINI_MESSAGE_SERVER_CONTENT:
        handled = gemini_audio_process_server_message(json, len, generation);
        break;

    case GEMINI_MESSAGE_SESSION_RESUMPTION: {
        cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
        const bool resumable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(update, "resumable"));
        if (resumable && json_string(update, "newHandle", s_resume_handle, sizeof(s_resume_handle)) && s_resume_handle[0]) {
            s_resume_available = true;
            ESP_LOGI(TAG, "Gemini session resumption handle updated");
        }
        break;
    }

    case GEMINI_MESSAGE_GOAWAY:
        parse_goaway(root);
        ESP_LOGW(TAG, "Gemini GoAway timeLeft=%llums", (unsigned long long)s_goaway_ms);
        break;

    case GEMINI_MESSAGE_ERROR:
        ESP_LOGE(TAG, "GEMINI_PROTO: SERVER ERROR RAW: %.*s",
                 (int)(len < 512U ? len : 512U), json);
        audio_engine_notify(AUDIO_ENGINE_EVENT_ERROR, generation);
        handled = false;
        break;

    default:
        ESP_LOGW(TAG, "GEMINI_PROTO: message type unknown len=%u", (unsigned)len);
        handled = false;
        break;
    }

    if (s_greeting_sent && !s_greeting_finished && type == GEMINI_MESSAGE_SERVER_CONTENT) {
        cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
        const bool done = cJSON_IsObject(server) &&
            (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "generationComplete")) ||
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "turnComplete")));
        if (done) {
            s_greeting_finished = true;
            ESP_LOGI(TAG, "WS_GEMINI: Greeting selesai");
            ESP_LOGI(TAG, "WEBSOCKET: Greeting selesai -> MIC streaming ENABLED");
            audio_engine_start_input_session();
        }
    }

    cJSON_Delete(root);
    return handled;
}

bool gemini_protocol_setup_complete(void) { return s_setup_complete; }
bool gemini_protocol_greeting_finished(void) { return s_greeting_finished; }
bool gemini_protocol_should_resume(void) { return s_resume_available; }
bool gemini_protocol_take_resume_request(void)
{
    if (s_resume_attempted || !s_resume_available || s_goaway_ms == 0) return false;
    s_resume_attempted = true;
    return true;
}
uint64_t gemini_protocol_goaway_time_left_ms(void) { return s_goaway_ms; }
