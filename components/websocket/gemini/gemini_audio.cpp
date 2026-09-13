#include "gemini_audio.h"
#include "audio_engine.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";

bool gemini_audio_process_server_message(const char *json, size_t len, uint32_t generation)
{
    if (!json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (!cJSON_IsObject(server)) {
        cJSON_Delete(root);
        return false;
    }

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
            const size_t capacity = (b64_len / 4U) * 3U + 4U;
            uint8_t *pcm = static_cast<uint8_t *>(malloc(capacity));
            if (!pcm) {
                ESP_LOGE(TAG, "Decode buffer gagal: %u byte", (unsigned)capacity);
                continue;
            }

            size_t decoded_len = 0;
            const int rc = mbedtls_base64_decode(
                pcm, capacity, &decoded_len,
                reinterpret_cast<const unsigned char *>(encoded->valuestring), b64_len);

            ESP_LOGI(TAG,
                     "GEMINI_AUDIO: mimeType=%s base64_len=%u decoded_pcm_bytes=%u pcm_samples=%u",
                     mime_type, (unsigned)b64_len, (unsigned)decoded_len,
                     (unsigned)(decoded_len / 2U));

            if (rc != 0 || decoded_len == 0 || (decoded_len & 1U) != 0) {
                ESP_LOGW(TAG, "GEMINI_AUDIO: Base64/PCM16 invalid rc=%d decoded=%u",
                         rc, (unsigned)decoded_len);
                free(pcm);
                continue;
            }

            if (!audio_engine_push_model_audio(pcm, decoded_len, generation)) {
                ESP_LOGW(TAG, "GEMINI_AUDIO: AudioEngine reject PCM bytes=%u",
                         (unsigned)decoded_len);
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
            free(pcm);
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

    cJSON_Delete(root);
    return handled;
}
