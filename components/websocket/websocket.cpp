#include "websocket.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_config.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WEBSOCKET";
static esp_websocket_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_initialized = false;
static uint32_t s_generation = 0;
static constexpr TickType_t CONNECT_WAIT_TICKS = pdMS_TO_TICKS(15000);

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation);
extern "C" void websocket_gemini_on_disconnected(void);
extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation);
extern "C" bool websocket_gemini_setup_complete(void);
extern "C" bool websocket_gemini_should_resume(void);
extern "C" uint64_t websocket_gemini_goaway_time_left_ms(void);
extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len);
extern "C" bool websocket_gemini_send_audio_stream_end(esp_websocket_client_handle_t client);
extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text);

static void websocket_mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!pcm || len != 640U || !websocket_is_connected() || !websocket_setup_complete()) return;
    (void)websocket_send_audio(pcm, len);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_websocket_event_data_t *event = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ++s_generation;
        ESP_LOGI(TAG, "WebSocket connected, generation=%lu", (unsigned long)s_generation);
        if (!websocket_gemini_on_connected(s_client, s_generation)) {
            ESP_LOGE(TAG, "Gemini setup send failed");
            s_connected = false;
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            break;
        }
        audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED, s_generation);
        ESP_LOGI(TAG, "Waiting for Gemini setupComplete before MIC streaming");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (s_connected && event && event->data_ptr && event->data_len > 0) {
            websocket_gemini_on_data((const uint8_t *)event->data_ptr,
                                     (size_t)event->data_len,
                                     event->op_code, s_generation);
            if (websocket_gemini_setup_complete() && !audio_engine_input_session_active()) {
                audio_engine_start_input_session();
                ESP_LOGI(TAG, "Gemini setupComplete -> MIC streaming ENABLED");
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (audio_engine_input_session_active() && s_client)
            (void)websocket_gemini_send_audio_stream_end(s_client);
        audio_engine_stop_input_session();
        s_connected = false;
        websocket_gemini_on_disconnected();
        ESP_LOGW(TAG, "WebSocket disconnected");
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
    if (!web_config_load_api_key(api_key, sizeof(api_key))) return false;
    if (!web_config_api_key_is_valid(api_key)) return false;
    const int n = snprintf(uri, uri_len,
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",
        api_key);
    return n > 0 && (size_t)n < uri_len;
}

void websocket_init(void)
{
    if (s_initialized) return;
    if (!audio_engine_set_mic_sink(websocket_mic_sink, nullptr)) return;
    char uri[512] = {0};
    if (!build_server_uri(uri, sizeof(uri))) return;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.network_timeout_ms = 15000;
    cfg.reconnect_timeout_ms = 5000;
    cfg.disable_auto_reconnect = true;
    cfg.task_stack = 4096;

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return;
    if (esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, nullptr) != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
        return;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "WebSocket transport ready; audio remains in Audio Engine/HAL");
}

bool websocket_connect(void)
{
    if (!s_initialized) websocket_init();
    if (!s_client) return false;
    if (s_connected) return true;
    if (esp_websocket_client_start(s_client) != ESP_OK) return false;
    const TickType_t started = xTaskGetTickCount();
    while (!s_connected) {
        if ((xTaskGetTickCount() - started) >= CONNECT_WAIT_TICKS) {
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

void websocket_disconnect(void)
{
    if (!s_client) return;
    if (s_connected && audio_engine_input_session_active())
        (void)websocket_gemini_send_audio_stream_end(s_client);
    s_connected = false;
    audio_engine_stop_input_session();
    (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
}

bool websocket_is_connected(void) { return s_connected && s_client != nullptr; }

bool websocket_send_audio(const uint8_t *data, size_t length)
{
    if (!data || length != 640U || !websocket_is_connected() || !websocket_setup_complete()) return false;
    return websocket_gemini_send_audio(s_client, data, length);
}

bool websocket_send_text(const char *text)
{
    if (!text || !*text || !websocket_is_connected()) return false;
    return websocket_gemini_send_text(s_client, text);
}

bool websocket_setup_complete(void) { return websocket_is_connected() && websocket_gemini_setup_complete(); }
bool websocket_should_resume(void) { return websocket_gemini_should_resume(); }

bool websocket_take_resume_request(void)
{
    /* A stored handle is not enough to trigger resume. Only a GoAway event
     * requests an immediate reconnect; ordinary disconnect returns to WakeWord. */
    return websocket_gemini_should_resume() && websocket_gemini_goaway_time_left_ms() > 0;
}

uint64_t websocket_goaway_time_left_ms(void) { return websocket_gemini_goaway_time_left_ms(); }
