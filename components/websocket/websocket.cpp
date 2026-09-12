#include "websocket.h"
#include "audio_engine.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "web_config.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "WEBSOCKET";
static esp_websocket_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_initialized = false;
static uint32_t s_generation = 0;

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation);
extern "C" void websocket_gemini_on_disconnected(void);
extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation);
extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len);
extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text);

static void websocket_mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!pcm || len == 0 || !websocket_is_connected()) return;
    (void)websocket_send_audio(pcm, len);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *event = static_cast<esp_websocket_event_data_t *>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ++s_generation;
        ESP_LOGI(TAG, "WebSocket TERHUBUNG ke Gemini, generation=%lu", (unsigned long)s_generation);
        if (!websocket_gemini_on_connected(s_client, s_generation)) {
            ESP_LOGE(TAG, "Gemini setup gagal setelah WebSocket connected");
            s_connected = false;
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            break;
        }
        audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED, s_generation);
        audio_engine_start_input_session();
        ESP_LOGI(TAG, "Audio Engine MIC session aktif -> WebSocket");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (s_connected && event && event->data_ptr && event->data_len > 0) {
            websocket_gemini_on_data(static_cast<const uint8_t *>(event->data_ptr),
                                     static_cast<size_t>(event->data_len),
                                     event->op_code, s_generation);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        audio_engine_stop_input_session();
        ESP_LOGW(TAG, "WebSocket TERPUTUS");
        websocket_gemini_on_disconnected();
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket ERROR");
        break;

    default:
        break;
    }
}

static bool build_server_uri(char *uri, size_t uri_len)
{
    if (!uri || uri_len < 16) return false;
    char api_key[128] = {0};
    if (!web_config_load_api_key(api_key, sizeof(api_key))) {
        ESP_LOGE(TAG, "API key Gemini tidak ditemukan di NVS");
        return false;
    }
    if (!web_config_api_key_is_valid(api_key)) {
        ESP_LOGE(TAG, "API key Gemini tidak valid");
        return false;
    }
    const int n = snprintf(uri, uri_len,
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",
        api_key);
    return n > 0 && static_cast<size_t>(n) < uri_len;
}

void websocket_init(void)
{
    if (s_initialized) return;
    if (!audio_engine_set_mic_sink(websocket_mic_sink, nullptr)) {
        ESP_LOGE(TAG, "Gagal memasang MIC sink AudioEngine -> WebSocket");
        return;
    }

    char uri[512] = {0};
    if (!build_server_uri(uri, sizeof(uri))) {
        ESP_LOGE(TAG, "WebSocket init gagal: URI Gemini tidak tersedia");
        return;
    }

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.network_timeout_ms = 15000;
    cfg.reconnect_timeout_ms = 5000;
    cfg.disable_auto_reconnect = true;

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init gagal");
        return;
    }
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register WebSocket event gagal: 0x%x", (unsigned)err);
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
        return;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "WebSocket transport siap | AudioEngine -> WebSocket -> Gemini");
}

bool websocket_connect(void)
{
    if (!s_initialized) websocket_init();
    if (!s_client) return false;
    if (s_connected) return true;
    const esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start gagal: 0x%x", (unsigned)err);
        return false;
    }
    return true;
}

void websocket_disconnect(void)
{
    if (!s_client) return;
    s_connected = false;
    audio_engine_stop_input_session();
    (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
}

bool websocket_is_connected(void)
{
    return s_connected && s_client != nullptr;
}

bool websocket_send_audio(const uint8_t *data, size_t length)
{
    if (!data || length == 0 || !websocket_is_connected()) return false;
    return websocket_gemini_send_audio(s_client, data, length);
}

bool websocket_send_text(const char *text)
{
    if (!text || text[0] == '\0' || !websocket_is_connected()) return false;
    return websocket_gemini_send_text(s_client, text);
}
