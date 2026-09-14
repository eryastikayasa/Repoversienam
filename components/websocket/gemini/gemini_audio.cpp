#include "gemini_audio.h"
#include "audio_engine.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";

// RX transport accepts up to 64 KiB JSON payloads. A base64 field cannot
// decode to more than ceil(64 KiB * 3 / 4), plus a small safety margin.
static constexpr size_t DECODE_BUFFER_BYTES = (64U * 1024U * 3U) / 4U + 4U;
static uint8_t *s_decode_buffer = nullptr;

static bool ensure_decode_buffer()
{
    if (s_decode_buffer) return true;
    s_decode_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(DECODE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_decode_buffer) {
        ESP_LOGE(TAG, "Reusable decode buffer allocation gagal: %u byte", (unsigned)DECODE_BUFFER_BYTES);
        return false;
    }
    ESP_LOGI(TAG, "Reusable Base64 decode buffer siap: %u byte PSRAM", (unsigned)DECODE_BUFFER_BYTES);
    return true;
}

static bool process_server_root(const cJSON *root, uint32_t generation)
{
    if (!root) return false;

    const int64_t extract_start_us = esp_timer_get_time();
    cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (!cJSON_IsObject(server)) return false;

    bool handled = false;

    cJSON *interrupted = cJSON_GetObjectItemCaseSensitive(server, "interrupted");
    if (cJSON_IsTrue(interrupted)) {
        ESP_LOGW(TAG, "Gemini interrupted -> AudioEngine playback interrupt");
        audio_engine_notify(AUDIO_ENGINE_EVENT_INTERRUPT, generation);
        handled = true;
    }

    cJSON *model_turn = cJSON_GetObjectItemCaseSensitive(server, "modelTurn");
    cJSON *parts = cJSON_IsObject(model_turn)
        ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
    const uint32_t extract_us = (uint32_t)(esp_timer_get_time() - extract_start_us);

    static int64_t profile_last_us = 0;
    static uint64_t profile_count = 0;
    static uint64_t profile_extract_us = 0;
    static uint64_t profile_decode_us = 0;
    static uint64_t profile_enqueue_us = 0;
    static uint64_t profile_total_audio_us = 0;
    static uint64_t profile_chunks = 0;

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

            const char *mime_type = (cJSON_IsString(mime) && mime->valuestring)
                ? mime->valuestring : "<missing>";
            if (strncmp(mime_type, "audio/pcm", strlen("audio/pcm")) != 0) {
                ESP_LOGW(TAG, "inlineData mimeType bukan audio/pcm: %s", mime_type);
                continue;
            }

            const size_t b64_len = strlen(encoded->valuestring);
            const size_t max_decoded = (b64_len / 4U) * 3U + 4U;
            if (b64_len == 0 || max_decoded > DECODE_BUFFER_BYTES) {
                ESP_LOGW(TAG, "Base64 chunk terlalu besar: b64=%u max_decoded=%u buffer=%u",
                         (unsigned)b64_len, (unsigned)max_decoded, (unsigned)DECODE_BUFFER_BYTES);
                continue;
            }
            if (!ensure_decode_buffer()) continue;

            const int64_t decode_start_us = esp_timer_get_time();
            size_t decoded_len = 0;
            const int rc = mbedtls_base64_decode(
                s_decode_buffer, DECODE_BUFFER_BYTES, &decoded_len,
                reinterpret_cast<const unsigned char *>(encoded->valuestring), b64_len);
            const uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start_us);

            if (rc != 0 || decoded_len == 0 || (decoded_len & 1U) != 0) {
                ESP_LOGW(TAG, "GEMINI_AUDIO: Base64/PCM16 invalid rc=%d decoded=%u decode_us=%lu",
                         rc, (unsigned)decoded_len, (unsigned long)decode_us);
                continue;
            }

            const int64_t enqueue_start_us = esp_timer_get_time();
            const bool queued = audio_engine_push_model_audio(s_decode_buffer, decoded_len, generation);
            const uint32_t enqueue_us = (uint32_t)(esp_timer_get_time() - enqueue_start_us);

            ++profile_chunks;
            profile_extract_us += extract_us;
            profile_decode_us += decode_us;
            profile_enqueue_us += enqueue_us;

            if (!queued) {
                ESP_LOGW(TAG, "GEMINI_AUDIO: AudioEngine reject PCM bytes=%u decode_us=%lu enqueue_us=%lu",
                         (unsigned)decoded_len, (unsigned long)decode_us, (unsigned long)enqueue_us);
            } else {
                const audio_engine_turn_t *turn = audio_engine_get_turn();
                if (turn && turn->bytes_queued == decoded_len) {
                    ESP_LOGI(TAG, "AUDIO_ENGINE PCM QUEUED: rx=%llu queued=%llu pending=%uB",
                             (unsigned long long)turn->bytes_received,
                             (unsigned long long)turn->bytes_queued,
                             (unsigned)turn->pending_bytes);
                }
                audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_AUDIO, generation);
                handled = true;
            }
        }
    }

    const bool generation_complete = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(server, "generationComplete"));
    const bool turn_complete = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(server, "turnComplete"));
    if (generation_complete || turn_complete) {
        ESP_LOGI(TAG, "GEMINI_AUDIO: Turn complete");
        audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, generation);
        handled = true;
    }

    const int64_t now_us = esp_timer_get_time();
    if (!profile_last_us) profile_last_us = now_us;
    profile_total_audio_us += (uint64_t)(now_us - extract_start_us);
    ++profile_count;
    if (now_us - profile_last_us >= 5000000LL) {
        if (profile_chunks) {
            ESP_LOGI(TAG, "RX_PROFILE: chunks=%llu extract=%llu us avg base64=%llu us avg audio_enqueue=%llu us avg total_audio=%llu us avg",
                     (unsigned long long)profile_chunks,
                     (unsigned long long)(profile_extract_us / profile_chunks),
                     (unsigned long long)(profile_decode_us / profile_chunks),
                     (unsigned long long)(profile_enqueue_us / profile_chunks),
                     (unsigned long long)(profile_total_audio_us / profile_count));
        }
        profile_last_us = now_us;
        profile_count = 0;
        profile_extract_us = 0;
        profile_decode_us = 0;
        profile_enqueue_us = 0;
        profile_total_audio_us = 0;
        profile_chunks = 0;
    }

    return handled;
}

bool gemini_audio_process_server_root(const cJSON *root, uint32_t generation)
{
    return process_server_root(root, generation);
}

bool gemini_audio_process_server_message(const char *json, size_t len, uint32_t generation)
{
    if (!json || len == 0) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    const bool handled = process_server_root(root, generation);
    cJSON_Delete(root);
    return handled;
}
