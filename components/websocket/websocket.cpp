#include "websocket.h"
#include "websocket_transport.h"
#include "websocket_audio.h"
#include "gemini_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WEBSOCKET";
static bool s_initialized = false;
static constexpr TickType_t CONNECT_WAIT_TICKS = pdMS_TO_TICKS(15000);

void websocket_init(void)
{
    if (s_initialized) return;
    if (websocket_transport_init() != ESP_OK) return;
    websocket_audio_init();
    s_initialized = true;
    ESP_LOGI(TAG, "WebSocket facade ready; Gemini and audio remain in separate layers");
}

bool websocket_connect(void)
{
    if (!s_initialized) websocket_init();
    if (websocket_transport_connect() != ESP_OK) return false;
    const TickType_t started = xTaskGetTickCount();
    while (!websocket_transport_is_connected()) {
        if ((xTaskGetTickCount() - started) >= CONNECT_WAIT_TICKS) {
            (void)websocket_transport_disconnect();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

void websocket_disconnect(void)
{
    (void)websocket_transport_disconnect();
}

bool websocket_is_connected(void)
{
    return websocket_transport_is_connected();
}

bool websocket_send_audio(const uint8_t *data, size_t length)
{
    return websocket_audio_send_frame(data, length);
}

bool websocket_send_text(const char *text)
{
    if (!text || !*text) return false;
    return websocket_transport_send_text(text, strlen(text)) == ESP_OK;
}

bool websocket_setup_complete(void)
{
    return websocket_is_connected() && gemini_protocol_setup_complete();
}

bool websocket_should_resume(void)
{
    return gemini_protocol_should_resume();
}

bool websocket_take_resume_request(void)
{
    return gemini_protocol_take_resume_request();
}

uint64_t websocket_goaway_time_left_ms(void)
{
    return gemini_protocol_goaway_time_left_ms();
}
