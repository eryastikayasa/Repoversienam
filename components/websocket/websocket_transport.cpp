#include "websocket_transport.h"
#include "websocket_event.h"
#include "web_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_TRANSPORT";
static esp_websocket_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static bool s_initialized = false;
static uint32_t s_generation = 0;

/* Match Repo3's proven audio TX timing: 3000 ms write budget, one retry,
 * 30 ms retry delay. Both the current direct mic_tx sender and the historical
 * mic_net_tx sender use this profile; normal control messages stay at 2000 ms. */
static constexpr TickType_t MIC_SEND_TIMEOUT = pdMS_TO_TICKS(3000);
static constexpr TickType_t NORMAL_SEND_TIMEOUT = pdMS_TO_TICKS(2000);
static constexpr TickType_t MIC_SEND_RETRY_DELAY = pdMS_TO_TICKS(30);
static constexpr int MIC_SEND_RETRIES = 1;
static constexpr size_t API_KEY_MAX = 128;
static constexpr size_t URL_MAX = 512;

static bool build_server_url(char *url, size_t url_size)
{
    if (!url || url_size == 0) return false;
    char api_key[API_KEY_MAX] = {0};
    if (!web_config_load_api_key(api_key, sizeof(api_key)) || !web_config_api_key_is_valid(api_key)) {
        ESP_LOGE(TAG, "Gemini API key tidak tersedia/valid"); return false;
    }
    const int written = snprintf(url, url_size,
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s", api_key);
    memset(api_key, 0, sizeof(api_key));
    return written > 0 && (size_t)written < url_size;
}

esp_err_t websocket_transport_init(void)
{
    if (s_initialized) return ESP_OK;
    s_client = nullptr; s_connected = false; s_initialized = true;
    ESP_LOGI(TAG, "Transport WebSocket siap"); return ESP_OK;
}
esp_err_t websocket_transport_connect(void)
{
    if (!s_initialized) { esp_err_t err = websocket_transport_init(); if (err != ESP_OK) return err; }
    if (s_client && esp_websocket_client_is_connected(s_client)) { s_connected = true; return ESP_OK; }
    if (s_client) { (void)esp_websocket_client_destroy(s_client); s_client = nullptr; s_connected = false; }
    char url[URL_MAX] = {0};
    if (!build_server_url(url, sizeof(url))) return ESP_ERR_INVALID_ARG;
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = false;
    cfg.cert_common_name = "generativelanguage.googleapis.com";
    cfg.network_timeout_ms = 15000;
    cfg.reconnect_timeout_ms = 5000;
    cfg.disable_auto_reconnect = true;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.task_stack = 4096;
    cfg.buffer_size = 8192;
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "esp_websocket_client_init gagal"); return ESP_FAIL; }
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, nullptr);
    if (err != ESP_OK) { (void)esp_websocket_client_destroy(s_client); s_client = nullptr; return err; }
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) { (void)esp_websocket_client_destroy(s_client); s_client = nullptr; return err; }
    return ESP_OK;
}
esp_err_t websocket_transport_disconnect(void)
{
    s_connected = false; if (!s_client) return ESP_OK;
    return esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
}
esp_err_t websocket_transport_abort(void)
{
    s_connected = false;
    if (!s_client) return ESP_OK;
    return esp_websocket_client_stop(s_client);
}
bool websocket_transport_is_connected(void)
{
    return s_client && s_connected && esp_websocket_client_is_connected(s_client);
}
esp_websocket_client_handle_t websocket_transport_client(void) { return s_client; }
uint32_t websocket_transport_generation(void) { return s_generation; }

esp_err_t websocket_transport_send_text(const char *text, size_t len)
{
    if (!text || len == 0 || len > 8192) return ESP_ERR_INVALID_ARG;
    if (!websocket_transport_is_connected()) return ESP_ERR_INVALID_STATE;

    const char *task_name = pcTaskGetName(nullptr);
    const bool mic_sender = task_name &&
        (strcmp(task_name, "mic_tx") == 0 || strcmp(task_name, "mic_net_tx") == 0);
    const TickType_t timeout = mic_sender ? MIC_SEND_TIMEOUT : NORMAL_SEND_TIMEOUT;
    const int retries = mic_sender ? MIC_SEND_RETRIES : 0;

    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (!websocket_transport_is_connected()) return ESP_ERR_INVALID_STATE;
        if (attempt > 0) vTaskDelay(MIC_SEND_RETRY_DELAY);

        const int sent = esp_websocket_client_send_text(s_client, text, (int)len, timeout);
        if (sent == (int)len) return ESP_OK;

        ESP_LOGW(TAG, "WS send failed: attempt=%d sent=%d expected=%u timeout=%ums",
                 attempt + 1, sent, (unsigned)len,
                 (unsigned)(timeout * portTICK_PERIOD_MS));

        if (sent < 0 && !websocket_transport_is_connected()) return ESP_ERR_INVALID_STATE;
    }

    if (mic_sender) {
        ESP_LOGW(TAG, "MIC TX send failed after retry; preserving WebSocket session");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

void websocket_transport_event_connected(void)
{
    s_connected = true; ++s_generation;
    ESP_LOGI(TAG, "WebSocket CONNECTED generation=%lu", (unsigned long)s_generation);
}
void websocket_transport_event_disconnected(void)
{
    s_connected = false; ESP_LOGW(TAG, "WebSocket DISCONNECTED");
}
