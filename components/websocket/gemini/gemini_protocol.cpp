#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "websocket_transport.h"
#include "web_config.h"
#include "audio_engine.h"
#include "display_engine.h"
#include "display_text.h"
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

// Transcript state lives on the protocol RX path, not in display_engine.
// The display receives only the already-decoded text.
static char s_user_transcript[512] = {0};
static char s_user_interim[512] = {0};
static char s_gemini_transcript[512] = {0};
static bool s_user_turn_active = false;
static bool s_gemini_turn_active = false;
static bool s_user_needs_new_turn = true;

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

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1U;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void clear_user_transcript(void)
{
    s_user_transcript[0] = '\0';
    s_user_interim[0] = '\0';
    s_user_turn_active = false;
    display_text_set_user("");
}

static void clear_gemini_transcript(void)
{
    s_gemini_transcript[0] = '\0';
    s_gemini_turn_active = false;
    display_text_set_gemini("");
}

/*
 * Gemini Live emits server updates incrementally. Different transcription
 * updates can be cumulative ("halo" -> "halo apa") or fragment-like
 * ("halo" -> " apa"). Prefix-aware merging supports both without ever
 * duplicating the cumulative example into "halo halo apa".
 */
static void merge_transcript(char *dst, size_t cap, const char *incoming)
{
    if (!dst || cap == 0 || !incoming || !incoming[0]) return;

    while (*incoming == ' ' || *incoming == '\n' || *incoming == '\r' || *incoming == '\t') ++incoming;
    if (!incoming[0]) return;

    const size_t current_len = strlen(dst);
    const size_t incoming_len = strlen(incoming);
    if (current_len == 0) {
        copy_text(dst, cap, incoming);
        return;
    }

    if (strcmp(dst, incoming) == 0) return;

    // Cumulative/full partial: replace the previous hypothesis.
    if (incoming_len >= current_len && strncmp(incoming, dst, current_len) == 0) {
        copy_text(dst, cap, incoming);
        return;
    }

    // Older cumulative hypothesis arrived after a shorter update.
    if (current_len >= incoming_len && strncmp(dst, incoming, incoming_len) == 0) return;

    // Delta/fragment: append with a single separator when needed.
    size_t out = current_len;
    if (out + 1U < cap && out > 0 && dst[out - 1U] != ' ' && incoming[0] != ' ')
        dst[out++] = ' ';
    while (*incoming && out + 1U < cap) dst[out++] = *incoming++;
    dst[out] = '\0';
}

static void publish_user_text(void)
{
    char combined[512] = {0};
    copy_text(combined, sizeof(combined), s_user_transcript);
    if (s_user_interim[0]) merge_transcript(combined, sizeof(combined), s_user_interim);
    display_text_set_user(combined);
}

static void handle_input_transcription(const char *text, bool interim)
{
    if (!text || !text[0]) return;

    if (!s_user_turn_active) {
        if (s_user_needs_new_turn) {
            clear_user_transcript();
            s_user_needs_new_turn = false;
        }
        s_user_turn_active = true;
    }

    if (interim) {
        // Interim input is a replaceable hypothesis, not committed history.
        copy_text(s_user_interim, sizeof(s_user_interim), text);
    } else {
        // inputTranscription is authoritative/final for the spoken segment.
        merge_transcript(s_user_transcript, sizeof(s_user_transcript), text);
        s_user_interim[0] = '\0';
    }
    publish_user_text();
}

static void handle_output_transcription(const char *text)
{
    if (!text || !text[0]) return;

    if (!s_gemini_turn_active) {
        clear_gemini_transcript();
        s_gemini_turn_active = true;
    }
    merge_transcript(s_gemini_transcript, sizeof(s_gemini_transcript), text);
    display_text_set_gemini(s_gemini_transcript);
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
    cJSON *input_transcription = cJSON_CreateObject();
    cJSON *output_transcription = cJSON_CreateObject();
    if (!root || !setup || !generation || !modalities || !speech || !voice || !prebuilt || !realtime || !aad_config || !session || !input_transcription || !output_transcription) {
        cJSON_Delete(root); cJSON_Delete(setup); cJSON_Delete(generation); cJSON_Delete(modalities);
        cJSON_Delete(speech); cJSON_Delete(voice); cJSON_Delete(prebuilt); cJSON_Delete(realtime);
        cJSON_Delete(aad_config); cJSON_Delete(session); cJSON_Delete(input_transcription); cJSON_Delete(output_transcription);
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

    // Input transcription is already part of Repo6 setup. Enable output
    // transcription as well so OLED text can come only from Gemini's own
    // spoken output transcript.
    ok = ok && cJSON_AddItemToObject(setup, "inputAudioTranscription", input_transcription);
    ok = ok && cJSON_AddItemToObject(setup, "outputAudioTranscription", output_transcription);
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
    s_user_needs_new_turn = true;
    s_user_turn_active = false;
    s_gemini_turn_active = false;
    s_user_transcript[0] = '\0';
    s_user_interim[0] = '\0';
    s_gemini_transcript[0] = '\0';
    display_text_set_user("");
    display_text_set_gemini("");
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
    s_user_needs_new_turn = true;
    s_user_turn_active = false;
    s_gemini_turn_active = false;
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
            display_set_system_state(FACE_LISTENING, "Mendengarkan...");
            if (!send_greeting()) ESP_LOGW(TAG, "WS_GEMINI: Greeting JSON gagal dikirim");
        }
        break;
    case GEMINI_MESSAGE_SERVER_CONTENT: {
        cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
        if (!cJSON_IsObject(server)) {
            handled = gemini_audio_process_server_root(root, generation);
            break;
        }

        // These fields are the only source of conversation text for OLED.
        // They are handled here while the JSON is already parsed on the RX path.
        cJSON *interim_input = cJSON_GetObjectItemCaseSensitive(server, "interimInputTranscription");
        cJSON *input = cJSON_GetObjectItemCaseSensitive(server, "inputTranscription");
        cJSON *output = cJSON_GetObjectItemCaseSensitive(server, "outputTranscription");

        if (cJSON_IsObject(interim_input)) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(interim_input, "text");
            if (cJSON_IsString(text) && text->valuestring) handle_input_transcription(text->valuestring, true);
        }
        if (cJSON_IsObject(input)) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(input, "text");
            if (cJSON_IsString(text) && text->valuestring) handle_input_transcription(text->valuestring, false);
        }
        if (cJSON_IsObject(output)) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(output, "text");
            if (cJSON_IsString(text) && text->valuestring) handle_output_transcription(text->valuestring);
        }

        /* After the greeting turn, the next server-content event is the real
         * Gemini response path. Face THINKING is still driven by actual server
         * content; playback switches to SPEAKING when PCM reaches the speaker. */
        if (s_greeting_finished && !cJSON_IsObject(output)) {
            display_set_system_state(FACE_THINKING, "Berpikir...");
        }
        handled = gemini_audio_process_server_root(root, generation);

        const bool interrupted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "interrupted"));
        const bool generation_complete = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "generationComplete"));
        const bool turn_complete = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(server, "turnComplete"));
        if (interrupted) {
            s_gemini_turn_active = false;
            s_user_needs_new_turn = true;
        } else if (generation_complete || turn_complete) {
            s_gemini_turn_active = false;
            s_user_needs_new_turn = true;
        }
        break;
    }
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
        display_set_system_state(FACE_ERROR, "Gemini error");
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
