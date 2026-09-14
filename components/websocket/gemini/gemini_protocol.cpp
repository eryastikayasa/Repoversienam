#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "websocket_transport.h"
#include "web_config.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
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

static bool add_setup_string(cJSON *object, const char *key, const char *value)
{
    return object && key && value && cJSON_AddStringToObject(object, key, value) != nullptr;
}

static bool build_setup(void)
{
    s_setup_role[0] = '\0';
    const bool have_role = web_config_load_role(s_setup_role, sizeof(s_setup_role)) && s_setup_role[0];

    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_CreateObject();
    cJSON *generation = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    cJSON *speech = cJSON_CreateObject();
    cJSON *voice = cJSON_CreateObject();
    cJSON *prebuilt = cJSON_CreateObject();
    cJSON *realtime = cJSON_CreateObject();
    cJSON *aad_config = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    if (!root || !setup || !generation || !modalities || !speech || !voice || !prebuilt || !realtime || !aad_config || !session) {
        cJSON_Delete(root); cJSON_Delete(setup); cJSON_Delete(generation); cJSON_Delete(modalities);
        cJSON_Delete(speech); cJSON_Delete(voice); cJSON_Delete(prebuilt); cJSON_Delete(realtime);
        cJSON_Delete(aad_config); cJSON_Delete(session);
        return false;
    }

    bool ok = true;
    ok = ok && cJSON_AddItemToObject(root, "setup", setup);
    ok = ok && add_setup_string(setup, "model", "models/gemini-3.1-flash-live-preview");
    ok = ok && cJSON_AddItemToObject(setup, "generationConfig", generation);
    ok = ok && cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    ok = ok && cJSON_AddItemToObject(generation, "responseModalities", modalities);
    ok = ok && add_setup_string(speech, "languageCode", "id-ID");
    ok = ok && cJSON_AddItemToObject(speech, "voiceConfig", voice);
    ok = ok && cJSON_AddItemToObject(voice, "prebuiltVoiceConfig", prebuilt);
    ok = ok && add_setup_string(prebuilt, "voiceName", "Kore");
    ok = ok && cJSON_AddItemToObject(generation, "speechConfig", speech);

    cJSON *compression = cJSON_CreateObject();
    cJSON *sliding = cJSON_CreateObject();
    ok = ok && compression && sliding;
    ok = ok && cJSON_AddItemToObject(setup, "contextWindowCompression", compression);
    ok = ok && cJSON_AddItemToObject(compression, "slidingWindow", sliding);

    ok = ok && cJSON_AddItemToObject(setup, "realtimeInputConfig", realtime);
    ok = ok && cJSON_AddItemToObject(realtime, "automaticActivityDetection", aad_config);
    ok = ok && cJSON_AddBoolToObject(aad_config, "disabled", false);
    ok = ok && add_setup_string(aad_config, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
    ok = ok && cJSON_AddNumberToObject(aad_config, "prefixPaddingMs", 40);
    ok = ok && add_setup_string(aad_config, "endOfSpeechSensitivity", "END_SENSITIVITY_HIGH");
    ok = ok && cJSON_AddNumberToObject(aad_config, "silenceDurationMs", 500);

    if (have_role) {
        cJSON *instruction = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        ok = ok && instruction && parts && part;
        ok = ok && cJSON_AddItemToObject(setup, "systemInstruction", instruction);
        ok = ok && cJSON_AddItemToObject(instruction, "parts", parts);
        ok = ok && cJSON_AddItemToArray(parts, part);
        ok = ok && add_setup_string(part, "text", s_setup_role);
    }

    ok = ok && cJSON_AddItemToObject(setup, "inputAudioTranscription", cJSON_CreateObject());
    ok = ok && cJSON_AddItemToObject(setup, "sessionResumption", session);
    if (s_resume_available) ok = ok && add_setup_string(session, "handle", s_resume_handle);
    if (!ok) { cJSON_Delete(root); return false; }

    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) { cJSON_Delete(root); return false; }
    const size_t len = strlen(printed);
    if (len + 1U > sizeof(s_setup_json)) {
        ESP_LOGE(TAG, "Gemini setup JSON terlalu besar: %u byte", (unsigned)len);
        cJSON_free(printed); cJSON_Delete(root); return false;
    }
    memcpy(s_setup_json, printed, len + 1U);
    cJSON_free(printed);
    cJSON_Delete(root);

    cJSON *validation = cJSON_ParseWithLength(s_setup_json, len);
    if (!validation) {
        const char *error = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "Gemini setup JSON INVALID: %s", error ? error : "unknown cJSON error");
        return false;
    }
    cJSON_Delete(validation);
    ESP_LOGI(TAG, "Gemini setup JSON VALID (%u byte)", (unsigned)len);
    return true;
}

bool gemini_protocol_on_connected(void)
{
    s_setup_complete = false;
    s_greeting_sent = false;
    s_greeting_finished = false;
    s_resume_attempted = false;
    s_goaway_ms = 0;
    if (!build_setup()) { ESP_LOGE(TAG, "Gemini setup JSON gagal dibuat"); return false; }
    const size_t len = strlen(s_setup_json);
    if (websocket_transport_send_text(s_setup_json, len) != ESP_OK) { ESP_LOGE(TAG, "Gemini setup gagal dikirim"); return false; }
    ESP_LOGI(TAG, "Gemini setup sent");
    ESP_LOGI(TAG, "Gemini setup terkirim (%u byte)", (unsigned)len);
    ESP_LOGI(TAG, "Waiting for Gemini setupComplete before greeting");
    return true;
}

void gemini_protocol_on_disconnected(void)
{
    s_setup_complete = false; s_greeting_sent = false; s_greeting_finished = false;
    s_goaway_ms = 0; s_resume_attempted = false;
}

bool gemini_protocol_process_message(const char *json, size_t len, uint32_t generation)
{
    if (!json || len == 0) return false;

    const int64_t total_start_us = esp_timer_get_time();
    const int64_t parse_start_us = total_start_us;
    cJSON *root = cJSON_ParseWithLength(json, len);
    const uint32_t parse_us = (uint32_t)(esp_timer_get_time() - parse_start_us);
    if (!root) {
        ESP_LOGW(TAG, "Gemini RX JSON invalid len=%u", (unsigned)len);
        return false;
    }

    const int64_t classify_start_us = esp_timer_get_time();
    const gemini_message_type_t type = gemini_message_classify_root(root);
    const uint32_t classify_us = (uint32_t)(esp_timer_get_time() - classify_start_us);

    bool handled = true;
    switch (type) {
    case GEMINI_MESSAGE_SETUP:
        if (!s_setup_complete) {
            s_setup_complete = true;
            ESP_LOGI(TAG, "GEMINI_PROTO: Gemini setupComplete");
            ESP_LOGI("WEBSOCKET", "Gemini setupComplete - audio uplink READY");
            if (!send_greeting()) ESP_LOGW(TAG, "WS_GEMINI: Greeting JSON gagal dikirim");
        }
        break;
    case GEMINI_MESSAGE_SERVER_CONTENT:
        handled = gemini_audio_process_server_root(root, generation);
        break;
    case GEMINI_MESSAGE_SESSION_RESUMPTION: {
        cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
        const bool resumable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(update, "resumable"));
        if (resumable && json_string(update, "newHandle", s_resume_handle, sizeof(s_resume_handle)) && s_resume_handle[0]) {
            s_resume_available = true; ESP_LOGI(TAG, "Gemini session resumption handle updated");
        }
        break;
    }
    case GEMINI_MESSAGE_GOAWAY:
        parse_goaway(root); ESP_LOGW(TAG, "Gemini GoAway timeLeft=%llums", (unsigned long long)s_goaway_ms); break;
    case GEMINI_MESSAGE_ERROR:
        ESP_LOGE(TAG, "GEMINI_PROTO: SERVER ERROR RAW: %.*s", (int)(len < 512U ? len : 512U), json);
        audio_engine_notify(AUDIO_ENGINE_EVENT_ERROR, generation); handled = false; break;
    default:
        ESP_LOGW(TAG, "GEMINI_PROTO: message type unknown len=%u", (unsigned)len); handled = false; break;
    }

    if (s_greeting_sent && !s_greeting_finished && type == GEMINI_MESSAGE_SERVER_CONTENT) {
        cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
        const bool done = cJSON_IsObject(server) &&
            (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "generationComplete")) ||
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "turnComplete")));
        if (done) {
            s_greeting_finished = true;
            ESP_LOGI(TAG, "WS_GEMINI: Greeting server turn selesai; menunggu playback drain");
            ESP_LOGI(TAG, "WEBSOCKET: Greeting audio drain dulu -> MIC streaming ENABLED setelah drain");
            audio_engine_request_input_session_after_drain();
        }
    }

    const uint32_t total_us = (uint32_t)(esp_timer_get_time() - total_start_us);
    // Keep this profiling path sampled/aggregated so UART logging does not
    // become part of the realtime hot path.
    static int64_t profile_last_us = 0;
    static uint64_t profile_count = 0;
    static uint64_t profile_parse_us = 0;
    static uint64_t profile_classify_us = 0;
    static uint64_t profile_total_us = 0;
    ++profile_count;
    profile_parse_us += parse_us;
    profile_classify_us += classify_us;
    profile_total_us += total_us;
    const int64_t now_us = esp_timer_get_time();
    if (!profile_last_us) profile_last_us = now_us;
    if (now_us - profile_last_us >= 5000000LL) {
        ESP_LOGI(TAG, "RX_PROFILE: count=%llu json=%llu us avg classify=%llu us avg total=%llu us avg",
                 (unsigned long long)profile_count,
                 (unsigned long long)(profile_parse_us / profile_count),
                 (unsigned long long)(profile_classify_us / profile_count),
                 (unsigned long long)(profile_total_us / profile_count));
        profile_last_us = now_us;
        profile_count = 0;
        profile_parse_us = 0;
        profile_classify_us = 0;
        profile_total_us = 0;
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
    s_resume_attempted = true; return true;
}
uint64_t gemini_protocol_goaway_time_left_ms(void) { return s_goaway_ms; }
