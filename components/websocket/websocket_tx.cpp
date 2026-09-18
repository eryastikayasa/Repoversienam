#include "websocket_internal.h"
#include "web_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WS_TX";

const char *websocket_get_server_url(void)
{
    static char url[256];
    char api_key[128] = {0};

    if (!web_config_load_api_key(api_key, sizeof(api_key))) {
        ESP_LOGE(TAG, "Gemini API key tidak ditemukan di NVS");
        return NULL;
    }

    const size_t key_len = strlen(api_key);
    char prefix[5] = "";
    char suffix[5] = "";
    if (key_len >= 4) {
        memcpy(prefix, api_key, 4);
        memcpy(suffix, api_key + key_len - 4, 4);
        prefix[4] = '\0';
        suffix[4] = '\0';
    }

    ESP_LOGI(TAG, "API key runtime: len=%u prefix=%s suffix=%s valid=%s",
             (unsigned)key_len,
             prefix,
             key_len >= 4 ? suffix : "-",
             web_config_api_key_is_valid(api_key) ? "YES" : "NO");

    if (!web_config_api_key_is_valid(api_key)) {
        ESP_LOGE(TAG, "API key NVS tidak valid; WebSocket URL tidak dibuat");
        return NULL;
    }

    int n = snprintf(
        url, sizeof(url),
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",
        api_key);

    if (n < 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGE(TAG, "WebSocket URL terlalu panjang");
        return NULL;
    }

    return url;
}

void websocket_send_audio_data(
    const uint8_t *data,
    size_t len
)
{
    if (!data || len == 0) {
        return;
    }

    if (len > WS_TX_AUDIO_SIZE) {
        ESP_LOGE(
            TAG,
            "Audio frame terlalu besar: %u byte",
            (unsigned)len
        );
        return;
    }

    if (!is_connected ||
        !setup_complete ||
        websocket_tx_error) {
        return;
    }

    uint32_t generation =
        websocket_connection_generation;

    if (!websocket_tx_enqueue_audio(
            data,
            len,
            generation)) {
        ESP_LOGD(
            TAG,
            "Audio frame tidak masuk TX queue"
        );
    }
}
