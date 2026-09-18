#include "websocket_internal.h"
#include "web_config.h"
#include "display.h"
#include "audio_hal.h"
#include "uart_control.h"

#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WS_JSON";

/* v7.0.25: fixed PCM decode workspace. */
#define PCM_DECODE_WORKSPACE_SIZE (24 * 1024)
static uint8_t pcm_decode_buffer[PCM_DECODE_WORKSPACE_SIZE];

/* v7.0.28:
 * Large Gemini audio JSON contains a base64 string that can be 10-20 KB.
 * cJSON normally duplicates that string while building its tree, temporarily
 * consuming another large heap block. Before cJSON parses a large audio
 * message, decode the base64 directly from the RX slot into the persistent
 * PCM workspace, then remove only the base64 characters from the RX JSON.
 * The JSON structure is preserved, so the existing cJSON parser and handling
 * of turnComplete/sessionResumption/etc. remain unchanged, but cJSON no
 * longer needs to allocate a duplicate copy of the audio payload.
 */
static bool compact_large_audio_payload(char *json, size_t *io_len)
{
    if (!json || !io_len || *io_len == 0) return false;

    const size_t len = *io_len;
    const char *inline_key = strstr(json, "\"inlineData\"");
    if (!inline_key) return false;

    const char *data_key = strstr(inline_key, "\"data\"");
    if (!data_key || data_key >= json + len) return false;

    const char *p = data_key + strlen("\"data\"");
    const char *end = json + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != ':') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != '\"') return false;
    ++p;

    char *b64 = (char *)p;
    char *q = b64;
    while (q < end) {
        if (*q == '\\') return false;
        if (*q == '\"') break;
        ++q;
    }
    if (q >= end) return false;

    size_t b64_len = (size_t)(q - b64);
    if (b64_len < 4096) return false;

    size_t pcm_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                    (const unsigned char *)b64, b64_len);
    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return false;
    if (pcm_len == 0) return false;

    if (pcm_len > PCM_DECODE_WORKSPACE_SIZE) {
        ESP_LOGW(TAG, "PCM audio melebihi fixed workspace: need=%u capacity=%u - audio dilewati",
                 (unsigned)pcm_len, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
    } else {
        size_t decoded = pcm_len;
        ret = mbedtls_base64_decode(pcm_decode_buffer, sizeof(pcm_decode_buffer), &decoded,
                                    (const unsigned char *)b64, b64_len);
        if (ret != 0 || decoded == 0) return false;
        audio_chunks_received++;
        audio_bytes_received += decoded;
        ESP_LOGI(TAG, "AUDIO GEMINI: %u byte -> AUDIO BUFFER (PCM cap=%u, compact JSON)",
                 (unsigned)decoded, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
        if (!queue_audio_pcm(pcm_decode_buffer, decoded))
            ESP_LOGE(TAG, "Gagal memasukkan audio ke ring buffer");
    }

    const size_t remove_len = b64_len;
    memmove(b64, q, (size_t)(end - q));
    *io_len = len - remove_len;
    return true;
}

void clear_session_handle(void)
{
    session_handle[0] = '\0';
    session_resumable = false;
}

bool store_session_handle(const char *handle)
{
    if (!handle || handle[0] == '\0') return false;
    size_t len = strlen(handle);
    if (len >= sizeof(session_handle)) {
        ESP_LOGE(TAG, "Session handle terlalu panjang: %u", (unsigned)len);
        return false;
    }
    memcpy(session_handle, handle, len + 1);
    session_resumable = true;
    ESP_LOGI(TAG, "Session resumption handle tersimpan: %u byte", (unsigned)len);
    return true;
}

static void add_device_control_tool(cJSON *setup)
{
    if (!setup) return;

    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    if (!tools) return;

    cJSON *tool = cJSON_CreateObject();
    cJSON *functions = cJSON_AddArrayToObject(tool, "functionDeclarations");
    cJSON *decl = cJSON_CreateObject();
    cJSON_AddStringToObject(decl, "name", "control_device");
    cJSON_AddStringToObject(decl, "description",
        "Mengontrol perangkat rumah melalui UART. Gunakan hanya jika pengguna meminta aksi perangkat. Setelah aksi berhasil, jawab pengguna secara natural dalam bahasa Indonesia dan jangan menyebut nama command UART.");

    cJSON *parameters = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(parameters, "type", "OBJECT");
    cJSON *properties = cJSON_AddObjectToObject(parameters, "properties");
    cJSON *command = cJSON_AddObjectToObject(properties, "command");
    cJSON_AddStringToObject(command, "type", "STRING");
    cJSON_AddStringToObject(command, "description", "Command perangkat yang harus dijalankan sebagai satu kali tekan tombol fisik.");

    cJSON *enum_values = cJSON_AddArrayToObject(command, "enum");
    static const char *const commands[] = {
        "r1", "r2", "r3", "r4",
        "fan_pwr", "fan_speed", "fan_swing", "fan_mode",
        "mp3_mode", "mp3_play", "mp3_eq",
        "m_led", "m_mute", "m_musik", "m_cek",
        "cek_suhu", "cek_cahaya",
        // Gemini Face commands
        "face_idle",
        "face_listening",
        "face_thinking",
        "face_speaking",
        "face_happy",
        "face_sad",
        "face_error",
        "face_sleep"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        cJSON_AddItemToArray(enum_values, cJSON_CreateString(commands[i]));

    cJSON *required = cJSON_AddArrayToObject(parameters, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(functions, decl);
    cJSON_AddItemToArray(tools, tool);
}

bool build_gemini_setup(char **output, size_t *output_len)
{
    if (!output || !output_len) return false;
    *output = NULL;
    *output_len = 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *generation_config = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(generation_config, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    cJSON *speech_config = cJSON_AddObjectToObject(generation_config, "speechConfig");
    cJSON_AddStringToObject(speech_config, "languageCode", "id-ID");
    cJSON *voice_config = cJSON_AddObjectToObject(speech_config, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice_config, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");
    cJSON_AddStringToObject(setup, "model", "models/gemini-3.1-flash-live-preview");
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");

    cJSON *system_instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *system_parts = cJSON_AddArrayToObject(system_instruction, "parts");
    cJSON *system_text = cJSON_CreateObject();
    cJSON_AddStringToObject(system_text, "text",
            "Kamu adalah asisten suara berbahasa Indonesia. "
    "Jika pengguna meminta aksi pada perangkat, gunakan fungsi control_device dengan command tombol yang tepat. "
    "Semua aksi perangkat berarti menekan tombol fisik satu kali. Kamu tidak mengetahui dan tidak boleh menebak keadaan fisik perangkat, serta tidak menyimpan atau melacak status ON/OFF. "
    "Pahami perintah natural seperti \"tekan tombol play\", \"hidupkan kipas\", \"matikan kipas\", \"naikkan kecepatan kipas\", \"turunkan kecepatan kipas\", \"ubah mode kipas\", \"tekan tombol colokan harian\", \"tekan tombol colokan panjang\", \"tekan tombol saklar lampu\", dan \"tekan tombol power MP3\". "
    "Pemetaan tombol: colokan harian=r1, colokan panjang=r2, saklar lampu=r3, power MP3=r4, fan_pwr=tombol power kipas untuk mematikan kipas, fan_speed=tombol speed kipas untuk menghidupkan kipas dan mengubah kecepatan, fan_swing=ayunan kipas, fan_mode=mode kipas, mode MP3=mp3_mode, play=mp3_play, EQ=mp3_eq, LED=m_led, mute=m_mute, musik=m_musik, cek=m_cek. "
    "Jika pengguna mengatakan hidupkan kipas, gunakan fan_speed satu kali. Jika pengguna mengatakan matikan kipas, gunakan fan_pwr satu kali. Jika pengguna meminta naikkan atau turunkan kecepatan kipas, gunakan fan_speed satu kali. Jangan gunakan fan_pwr untuk menghidupkan kipas. "
    "Untuk relay, colokan, lampu, MP3, dan tombol lainnya, kata hidupkan, matikan, atau tekan tidak mengubah jumlah tekanan: selalu kirim command yang sesuai tepat satu kali. "
    "Setelah fungsi berhasil, respons mengikuti maksud pengguna secara natural. Jika pengguna mengatakan hidupkan, katakan bahwa sudah dihidupkan. Jika mengatakan matikan, katakan bahwa sudah dimatikan. Jika mengatakan tekan, katakan bahwa tombol sudah ditekan. Jangan mengklaim mengetahui status fisik perangkat. "
    "Jangan membuat atau menggunakan command on/off berbasis status. Jangan mengarang command. "
    "Jika pengguna meminta kamu menampilkan ekspresi wajah, gunakan control_device dengan command Face yang sesuai. "
    "Gunakan face_happy untuk senyum atau bahagia, "
    "face_sad untuk sedih atau menangis, "
    "face_thinking untuk berpikir, "
    "face_listening untuk mendengarkan, "
    "face_speaking untuk berbicara, "
    "face_error untuk kesalahan atau kaget, "
    "face_sleep untuk tidur, "
    "dan face_idle untuk ekspresi netral. "
    "Setiap command Face akan tampil selama 5 detik lalu kembali ke ekspresi sebelumnya. "
    "Tunggu hasil fungsi sebelum menyatakan tombol berhasil ditekan. "
    "Jangan pernah mengucapkan nama command UART kepada pengguna.");
    cJSON_AddItemToArray(system_parts, system_text);
    static char role_text[512];

if (web_config_load_role(role_text, sizeof(role_text)) &&
    role_text[0] != '\0') {
    cJSON *role_part = cJSON_CreateObject();
    if (role_part) {
        cJSON_AddStringToObject(role_part, "text", role_text);
        cJSON_AddItemToArray(system_parts, role_part);
    }
}
    add_device_control_tool(setup);

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", false);
    cJSON *resumption = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (session_resumable && session_handle[0] != '\0')
        cJSON_AddStringToObject(resumption, "handle", session_handle);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ESP_LOGE(TAG, "Gagal serialize Gemini setup JSON");
        return false;
    }
    *output = json;
    *output_len = strlen(json);
    ESP_LOGI(TAG, "Gemini setup V7.0.9: AUDIO + id-ID + AAD ENABLED + UART TOOL");
    return true;
}

static cJSON *parse_json_with_diagnostics(const char *json, size_t len)
{
    if (!json || len == 0) return NULL;
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, 0);
    if (root != NULL) return root;
    const char *error_ptr = cJSON_GetErrorPtr();
    size_t error_offset = 0;
    if (error_ptr && error_ptr >= json && error_ptr < json + len)
        error_offset = (size_t)(error_ptr - json);
    else if (parse_end && parse_end >= json && parse_end <= json + len)
        error_offset = (size_t)(parse_end - json);
    ESP_LOGW(TAG, "Payload bukan JSON valid: %u byte, error_offset=%u",
             (unsigned)len, (unsigned)error_offset);
    if (error_offset < len) {
        size_t start = error_offset > 24 ? error_offset - 24 : 0;
        size_t remaining = len - start;
        size_t preview_len = remaining < 96 ? remaining : 96;
        char preview[97];
        for (size_t i = 0; i < preview_len; ++i) {
            unsigned char c = (unsigned char)json[start + i];
            preview[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        preview[preview_len] = '\0';
        ESP_LOGW(TAG, "JSON sekitar error @%u: \"%s\"", (unsigned)error_offset, preview);
    }
    ESP_LOGW(TAG, "JSON RX diagnostic: heap=%u", (unsigned)esp_get_free_heap_size());
    return NULL;
}

static void process_gemini_tool_call(cJSON *tool_call)
{
    if (!cJSON_IsObject(tool_call)) return;

    cJSON *function_calls = cJSON_GetObjectItem(tool_call, "functionCalls");
    if (!cJSON_IsArray(function_calls)) return;

    cJSON *fc = NULL;
    cJSON_ArrayForEach(fc, function_calls) {
        if (!cJSON_IsObject(fc)) continue;

        cJSON *id = cJSON_GetObjectItem(fc, "id");
        cJSON *name = cJSON_GetObjectItem(fc, "name");
        cJSON *args = cJSON_GetObjectItem(fc, "args");
        if (!cJSON_IsString(id) || !id->valuestring ||
            !cJSON_IsString(name) || !name->valuestring ||
            !cJSON_IsObject(args)) {
            ESP_LOGW(TAG, "Tool call Gemini tidak lengkap");
            continue;
        }

        ESP_LOGI(TAG, "Gemini TOOL CALL: %s id=%s", name->valuestring, id->valuestring);

        bool success = false;
        if (strcmp(name->valuestring, "control_device") == 0) {
            cJSON *command = cJSON_GetObjectItem(args, "command");

            if (cJSON_IsString(command) && command->valuestring) {
                const char *cmd = command->valuestring;

                if (strcmp(cmd, "face_idle") == 0) {
                    face_show_for_ms(FACE_IDLE, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_listening") == 0) {
                    face_show_for_ms(FACE_LISTENING, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_thinking") == 0) {
                    face_show_for_ms(FACE_THINKING, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_speaking") == 0) {
                    face_show_for_ms(FACE_SPEAKING, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_happy") == 0) {
                    face_show_for_ms(FACE_HAPPY, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_sad") == 0) {
                    face_show_for_ms(FACE_SAD, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_error") == 0) {
                    face_show_for_ms(FACE_ERROR, 5000);
                    success = true;
                }
                else if (strcmp(cmd, "face_sleep") == 0) {
                    face_show_for_ms(FACE_SLEEP, 5000);
                    success = true;
                }
                else {
                    success = uart_control_execute_command(cmd);
                    ESP_LOGI(TAG, "UART TOOL command=%s result=%s", cmd, success ? "OK" : "REJECTED");
                }
            } else {
                ESP_LOGW(TAG, "control_device tanpa argument command");
            }
        }

        if (!websocket_send_tool_response(id->valuestring, name->valuestring, success))
            ESP_LOGW(TAG, "Gagal mengirim toolResponse ke Gemini");
    }
}

void process_gemini_message(const char *json, size_t len)
{
    if (!json || len == 0) return;
    size_t parse_len = len;
    (void)compact_large_audio_payload((char *)json, &parse_len);
    cJSON *root = parse_json_with_diagnostics(json, parse_len);
    if (!root) return;

    cJSON *setup_complete_obj = cJSON_GetObjectItem(root, "setupComplete");
    if (cJSON_IsObject(setup_complete_obj)) {
        setup_complete = true;
        ESP_LOGI(TAG, "Gemini setupComplete: SESI SIAP");
        display_status("AI Siap!");
        cJSON_Delete(root);
        return;
    }

    cJSON *tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (cJSON_IsObject(tool_call))
        process_gemini_tool_call(tool_call);

    cJSON *input_transcription = cJSON_GetObjectItem(root, "inputTranscription");
    if (cJSON_IsObject(input_transcription)) {
        cJSON *text = cJSON_GetObjectItem(input_transcription, "text");
        if (cJSON_IsString(text) && text->valuestring)
            ESP_LOGI(TAG, "USER: %s", text->valuestring);
    }

    cJSON *output_transcription = cJSON_GetObjectItem(root, "outputTranscription");
    if (cJSON_IsObject(output_transcription)) {
        cJSON *text = cJSON_GetObjectItem(output_transcription, "text");
        if (cJSON_IsString(text) && text->valuestring)
            ESP_LOGI(TAG, "GEMINI TEXT: %s", text->valuestring);
    }

    cJSON *server = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(server)) {
        cJSON *turn = cJSON_GetObjectItem(server, "modelTurn");
        if (cJSON_IsObject(turn)) {
            cJSON *parts = cJSON_GetObjectItem(turn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part = NULL;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *inlineData = cJSON_GetObjectItem(part, "inlineData");
                    if (!cJSON_IsObject(inlineData)) continue;
                    cJSON *mimeType = cJSON_GetObjectItem(inlineData, "mimeType");
                    if (cJSON_IsString(mimeType) && mimeType->valuestring)
                        ESP_LOGD(TAG, "Gemini audio MIME: %s", mimeType->valuestring);
                    cJSON *audio = cJSON_GetObjectItem(inlineData, "data");
                    if (!cJSON_IsString(audio) || !audio->valuestring) continue;
                    const char *b64 = audio->valuestring;
                    size_t b64_len = strlen(b64);
                    if (b64_len == 0) continue;
                    if (b64_len > (WS_RX_MAX_PAYLOAD_SIZE * 2)) {
                        ESP_LOGE(TAG, "Base64 audio terlalu besar: %u", (unsigned)b64_len);
                        continue;
                    }
                    size_t pcm_len = 0;
                    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                                    (const unsigned char *)b64, b64_len);
                    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) continue;
                    if (pcm_len == 0 || pcm_len > PCM_DECODE_WORKSPACE_SIZE) continue;
                    size_t decoded = pcm_len;
                    ret = mbedtls_base64_decode(pcm_decode_buffer, sizeof(pcm_decode_buffer),
                                                &decoded, (const unsigned char *)b64, b64_len);
                    if (ret == 0 && decoded > 0) {
                        audio_chunks_received++;
                        audio_bytes_received += decoded;
                        ESP_LOGI(TAG, "AUDIO GEMINI: %u byte -> AUDIO BUFFER (PCM cap=%u)",
                                 (unsigned)decoded, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
                        if (!queue_audio_pcm(pcm_decode_buffer, decoded))
                            ESP_LOGE(TAG, "Gagal memasukkan audio ke ring buffer");
                    }
                }
            }
        }

        cJSON *generation_complete = cJSON_GetObjectItem(server, "generationComplete");
        if (cJSON_IsTrue(generation_complete)) ESP_LOGI(TAG, "Gemini: GENERATION COMPLETE");

        cJSON *turn_complete = cJSON_GetObjectItem(server, "turnComplete");
        if (cJSON_IsTrue(turn_complete)) {
            audio_turn_complete_pending = true;
            size_t pending = get_audio_pending_bytes();
            ESP_LOGI(TAG, "Gemini: TURN COMPLETE - menunggu audio drain");
            ESP_LOGI(TAG, "AUDIO SUMMARY: chunks=%u received=%llu played=%llu pending=%u write_calls=%u",
                     (unsigned)audio_chunks_received, (unsigned long long)audio_bytes_received,
                     (unsigned long long)audio_bytes_played, (unsigned)pending,
                     (unsigned)audio_write_calls);
            if (pending == 0) check_audio_playback_complete();
        }

        cJSON *interrupted = cJSON_GetObjectItem(server, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            ESP_LOGW(TAG, "Gemini: RESPONSE INTERRUPTED");
            clear_audio_buffer();
            audio_turn_complete_pending = false;
            audio_turn_active = false;
        }
    }

    cJSON *resume = cJSON_GetObjectItem(root, "sessionResumptionUpdate");
    if (cJSON_IsObject(resume)) {
        cJSON *handle = cJSON_GetObjectItem(resume, "newHandle");
        cJSON *resumable = cJSON_GetObjectItem(resume, "resumable");
        bool can_resume = cJSON_IsTrue(resumable);
        if (can_resume && cJSON_IsString(handle) && handle->valuestring && handle->valuestring[0] != '\0') {
            if (store_session_handle(handle->valuestring))
                ESP_LOGI(TAG, "Session resumption: resumable=true, handle updated");
        } else {
            session_resumable = false;
            ESP_LOGI(TAG, "Session resumption: resumable=false");
        }
    }

    cJSON *go_away = cJSON_GetObjectItem(root, "goAway");
    if (cJSON_IsObject(go_away)) {
        ESP_LOGW(TAG, "Gemini mengirim GO AWAY");
        cJSON *time_left = cJSON_GetObjectItem(go_away, "timeLeft");
        if (cJSON_IsString(time_left) && time_left->valuestring)
            ESP_LOGW(TAG, "GO AWAY timeLeft: %s", time_left->valuestring);
    }

    cJSON_Delete(root);
}
