#include "audio_engine.h"

#include "esp_log.h"
#include "mbedtls/base64.h"

#include <stdint.h>
#include <stddef.h>

static const char *TAG = "AUDIO_INGEST";

static constexpr size_t DECODE_INPUT_QUAD_CHARS = 4;
static constexpr size_t DECODE_OUTPUT_BLOCK = 768;

extern "C" bool audio_engine_push_model_audio_base64(const char *b64, size_t len, uint32_t generation)
{
    if (!b64 || len == 0) return false;

    size_t quad_len = 0;
    unsigned char quad[DECODE_INPUT_QUAD_CHARS];
    uint8_t pcm[DECODE_OUTPUT_BLOCK + 1];
    uint8_t carry = 0;
    bool have_carry = false;
    bool pushed_any = false;

    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)b64[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

        quad[quad_len++] = c;
        if (quad_len != DECODE_INPUT_QUAD_CHARS) continue;

        size_t decoded = 0;
        const int ret = mbedtls_base64_decode(
            pcm + (have_carry ? 1 : 0), sizeof(pcm) - (have_carry ? 1 : 0),
            &decoded, quad, DECODE_INPUT_QUAD_CHARS);
        quad_len = 0;

        if (ret != 0 || decoded == 0) {
            ESP_LOGE(TAG, "Base64 audio decode gagal: ret=%d", ret);
            return false;
        }

        size_t total = decoded;
        if (have_carry) {
            pcm[0] = carry;
            ++total;
            have_carry = false;
        }

        if (total & 1U) {
            carry = pcm[total - 1];
            have_carry = true;
            --total;
        }

        if (total > 0) {
            if (!audio_engine_push_model_audio(pcm, total, generation)) {
                ESP_LOGW(TAG, "AudioEngine menolak PCM Base64 chunk: bytes=%u", (unsigned)total);
                return pushed_any;
            }
            pushed_any = true;
        }
    }

    if (quad_len != 0) {
        ESP_LOGW(TAG, "Base64 audio tidak lengkap: sisa=%u karakter", (unsigned)quad_len);
        return false;
    }

    if (have_carry) {
        ESP_LOGW(TAG, "Base64 audio menghasilkan byte PCM ganjil; frame tidak lengkap");
        return false;
    }

    return pushed_any;
}
