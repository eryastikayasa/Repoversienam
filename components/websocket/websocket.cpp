#include "websocket.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "web_config.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WEBSOCKET";
static esp_websocket_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_initialized = false;
static volatile bool s_resume_attempted = false;
static volatile bool s_greeting_sent = false;
static uint32_t s_generation = 0;
static constexpr TickType_t CONNECT_WAIT_TICKS = pdMS_TO_TICKS(15000);

extern "C" bool websocket_gemini_on_connected(esp_websocket_client_handle_t client, uint32_t generation);
extern "C" void websocket_gemini_on_disconnected(void);
extern "C" void websocket_gemini_on_data(const uint8_t *data, size_t len, int opcode, uint32_t generation);
extern "C" bool websocket_gemini_setup_complete(void);
extern "C" bool websocket_gemini_greeting_finished(void);
extern "C" bool websocket_gemini_should_resume(void);
extern "C" uint64_t websocket_gemini_goaway_time_left_ms(void);
extern "C" bool websocket_gemini_send_audio(esp_websocket_client_handle_t client, const uint8_t *data, size_t len);
extern "C" bool websocket_gemini_send_audio_stream_end(esp_websocket_client_handle_t client);
extern "C" bool websocket_gemini_send_text(esp_websocket_client_handle_t client, const char *text);

/* Gemini Live can split one WebSocket message across DATA callbacks.
 * Reconstruct the complete payload before handing it to the Gemini parser. */
static constexpr size_t RX_MAX_PAYLOAD = 64U * 1024U;
static constexpr size_t RX_BUFFER_COUNT = 3U;
static constexpr size_t RX_DIAGNOSTIC_MAX = 512U;
static constexpr uint32_t RX_WORKER_STACK = 8192U;
static constexpr UBaseType_t RX_WORKER_PRIORITY = 5U;

static QueueHandle_t s_rx_free_queue = nullptr;
static QueueHandle_t s_rx_ready_queue = nullptr;
static StaticQueue_t s_rx_free_queue_storage;
static StaticQueue_t s_rx_ready_queue_storage;
static char *s_rx_free_storage[RX_BUFFER_COUNT];
static char *s_rx_ready_storage[RX_BUFFER_COUNT];
static char *s_rx_buffers[RX_BUFFER_COUNT] = {};
static char *s_rx_assembling_buffer = nullptr;
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;
static bool s_rx_assembling = false;
static bool s_rx_text_message = false;
static TaskHandle_t s_rx_worker_task = nullptr;
static bool s_rx_worker_ready = false;

static void log_rx_hex(const char *label, const char *data, size_t len)
{
    if (!data || len == 0) {
        ESP_LOGI(TAG, "WS_RX: %s <empty>", label);
        return;
    }

    char hex[3 * 16 + 1] = {0};
    const size_t count = len < 16U ? len : 16U;
    size_t w = 0;
    for (size_t i = 0; i < count && w + 3U < sizeof(hex); ++i) {
        w += (size_t)snprintf(hex + w, sizeof(hex) - w, "%02X%s",
                              (unsigned char)data[i], i + 1U < count ? " " : "");
    }
    ESP_LOGI(TAG, "WS_RX: %s %s", label, hex);
}

static bool ensure_rx_worker(void)
{
    if (s_rx_worker_ready) return true;

    s_rx_free_queue = xQueueCreateStatic(RX_BUFFER_COUNT, sizeof(char *),
                                          reinterpret_cast<uint8_t *>(s_rx_free_storage),
                                          &s_rx_free_queue_storage);
    s_rx_ready_queue = xQueueCreateStatic(RX_BUFFER_COUNT, sizeof(char *),
                                           reinterpret_cast<uint8_t *>(s_rx_ready_storage),
                                           &s_rx_ready_queue_storage);
    if (!s_rx_free_queue || !s_rx_ready_queue) {
        ESP_LOGE(TAG, "WS_RX: gagal membuat RX queue");
        return false;
    }

    for (size_t i = 0; i < RX_BUFFER_COUNT; ++i) {
        s_rx_buffers[i] = static_cast<char *>(heap_caps_malloc(
            RX_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rx_buffers[i]) {
            ESP_LOGE(TAG, "WS_RX: gagal alokasi PSRAM buffer #%u", (unsigned)i);
            return false;
        }
        if (xQueueSend(s_rx_free_queue, &s_rx_buffers[i], 0) != pdPASS) {
            ESP_LOGE(TAG, "WS_RX: gagal isi free queue #%u", (unsigned)i);
            return false;
        }
    }

    if (xTaskCreate(
            [](void *) {
                ESP_LOGI(TAG, "WS_RX: worker START buffers=%u size=%uKB",
                         (unsigned)RX_BUFFER_COUNT,
                         (unsigned)(RX_MAX_PAYLOAD / 1024U));
                for (;;) {
                    char *json = nullptr;
                    if (xQueueReceive(s_rx_ready_queue, &json, portMAX_DELAY) != pdPASS)
                        continue;
                    if (!json) continue;

                    const size_t len = strlen(json);
                    ESP_LOGI(TAG, "WS_RX: RX message complete len=%u", (unsigned)len);
                    log_rx_hex("worker first bytes:", json, len);
                    if (len > 16U) log_rx_hex("worker last bytes:", json + len - 16U, 16U);
                    websocket_gemini_on_data(reinterpret_cast<const uint8_t *>(json),
                                             len, WEBSOCKET_EVENT_DATA, s_generation);

                    if (xQueueSend(s_rx_free_queue, &json, portMAX_DELAY) != pdPASS)
                        ESP_LOGE(TAG, "WS_RX: gagal mengembalikan RX buffer");
                }
            },
            "ws_rx", RX_WORKER_STACK, nullptr, RX_WORKER_PRIORITY, &s_rx_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "WS_RX: gagal membuat RX worker");
        return false;
    }

    s_rx_worker_ready = true;
    return true;
}

static void reset_rx(void)
{
    if (s_rx_assembling_buffer) {
        if (!s_rx_free_queue || xQueueSend(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS)
            ESP_LOGW(TAG, "WS_RX: assembly buffer gagal dikembalikan");
        s_rx_assembling_buffer = nullptr;
    }
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_assembling = false;
    s_rx_text_message = false;
}

static void handle_rx_data(esp_websocket_event_data_t *event)
{
    if (!event || !event->data_ptr || event->data_len <= 0 || event->payload_len <= 0) return;

    const size_t payload_len = (size_t)event->payload_len;
    const size_t payload_offset = (size_t)event->payload_offset;
    const size_t data_len = (size_t)event->data_len;
    const uint8_t opcode = event->op_code;
    const bool fin = event->fin;

    ESP_LOGI(TAG,
             "WS_RX: DATA opcode=0x%02X fin=%d payload_len=%u payload_offset=%u data_len=%u",
             opcode, fin ? 1 : 0, (unsigned)payload_len,
             (unsigned)payload_offset, (unsigned)data_len);
    log_rx_hex("first bytes:", event->data_ptr, data_len);
    if (data_len > 16U) log_rx_hex("last bytes:", event->data_ptr + data_len - 16U, 16U);

    if (payload_len > RX_MAX_PAYLOAD ||
        payload_offset > payload_len ||
        data_len > payload_len - payload_offset) {
        ESP_LOGW(TAG, "WS_RX: invalid boundary total=%u offset=%u len=%u",
                 (unsigned)payload_len, (unsigned)payload_offset, (unsigned)data_len);
        reset_rx();
        return;
    }

    /* RFC 6455 data opcodes: 0=continuation, 1=text, 2=binary.
     * Gemini Live protocol messages are JSON text. Never feed binary/control
     * payloads to the JSON parser. */
    if (opcode == 0x8U || opcode == 0x9U || opcode == 0xAU) {
        ESP_LOGI(TAG, "WS_RX: control frame opcode=0x%02X ignored", opcode);
        return;
    }
    if (opcode != 0x00U && opcode != 0x01U && opcode != 0x02U) {
        ESP_LOGW(TAG, "WS_RX: unsupported data opcode=0x%02X dropped", opcode);
        reset_rx();
        return;
    }

    if (!ensure_rx_worker()) {
        ESP_LOGW(TAG, "WS_RX: RX worker unavailable; payload dropped");
        reset_rx();
        return;
    }

    if (payload_offset == 0U) {
        reset_rx();

        if (opcode != 0x01U && opcode != 0x02U) {
            ESP_LOGW(TAG, "WS_RX: first message fragment opcode bukan TEXT/BINARY: 0x%02X", opcode);
            return;
        }

        if (xQueueReceive(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) {
            ESP_LOGW(TAG, "WS_RX: free queue empty; payload dropped");
            return;
        }
        s_rx_expected = payload_len;
        s_rx_received = 0;
        s_rx_assembling = true;
        s_rx_text_message = (opcode == 0x01U);
    } else if (!s_rx_assembling || s_rx_expected != payload_len ||
               payload_offset != s_rx_received || opcode != 0x00U) {
        ESP_LOGW(TAG,
                 "WS_RX: fragment sequence invalid opcode=0x%02X offset=%u received=%u expected=%u total=%u",
                 opcode, (unsigned)payload_offset, (unsigned)s_rx_received,
                 (unsigned)s_rx_expected, (unsigned)payload_len);
        reset_rx();
        return;
    }

    if (!s_rx_text_message) {
        ESP_LOGW(TAG, "WS_RX: BINARY Gemini message tidak diparse sebagai JSON; total=%u",
                 (unsigned)payload_len);
        reset_rx();
        return;
    }

    memcpy(s_rx_assembling_buffer + payload_offset, event->data_ptr, data_len);
    s_rx_received = payload_offset + data_len;

    if (fin && s_rx_received != s_rx_expected) {
        ESP_LOGW(TAG, "WS_RX: FIN sebelum payload lengkap received=%u expected=%u",
                 (unsigned)s_rx_received, (unsigned)s_rx_expected);
        reset_rx();
        return;
    }

    if (!fin && s_rx_received == s_rx_expected) {
        ESP_LOGW(TAG, "WS_RX: payload lengkap tetapi FIN=0; menunggu continuation");
        return;
    }

    if (s_rx_received != s_rx_expected) return;

    s_rx_assembling_buffer[s_rx_expected] = '\0';
    char *ready = s_rx_assembling_buffer;
    s_rx_assembling_buffer = nullptr;
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_assembling = false;
    s_rx_text_message = false;

    if (xQueueSend(s_rx_ready_queue, &ready, 0) != pdPASS) {
        ESP_LOGW(TAG, "WS_RX: ready queue penuh; payload dropped");
        if (xQueueSend(s_rx_free_queue, &ready, 0) != pdPASS)
            ESP_LOGE(TAG, "WS_RX: RX buffer hilang");
    }
}

static void websocket_task_audit(void)
{
    TaskHandle_t task = xTaskGetHandle("websocket_task");
    if (!task) {
        ESP_LOGW(TAG, "TASK AUDIT websocket_task: handle belum tersedia");
        return;
    }
    ESP_LOGI(TAG,
             "TASK AUDIT websocket_task stack=4096B watermark=%uB priority=%u core=%d",
             (unsigned)(uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t)),
             (unsigned)uxTaskPriorityGet(task), (int)xTaskGetCoreID(task));
}

static void websocket_mic_sink(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!pcm || len != 640U || !websocket_is_connected() ||
        !websocket_setup_complete() || !websocket_gemini_greeting_finished()) return;
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
        s_resume_attempted = false;
        s_greeting_sent = false;
        ++s_generation;
        ESP_LOGI(TAG, "WebSocket connected, generation=%lu", (unsigned long)s_generation);
        websocket_task_audit();
        ensure_rx_worker();
        if (!websocket_gemini_on_connected(s_client, s_generation)) {
            ESP_LOGE(TAG, "Gemini setup send failed");
            s_connected = false;
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            break;
        }
        audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED, s_generation);
        ESP_LOGI(TAG, "Waiting for Gemini setupComplete before greeting");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (s_connected) handle_rx_data(event);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (audio_engine_input_session_active() && s_client)
            (void)websocket_gemini_send_audio_stream_end(s_client);
        audio_engine_stop_input_session();
        s_connected = false;
        s_greeting_sent = false;
        reset_rx();
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
    websocket_task_audit();
    return true;
}

void websocket_disconnect(void)
{
    if (!s_client) return;
    if (s_connected && audio_engine_input_session_active())
        (void)websocket_gemini_send_audio_stream_end(s_client);
    s_connected = false;
    s_greeting_sent = false;
    reset_rx();
    audio_engine_stop_input_session();
    (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
}

bool websocket_is_connected(void) { return s_connected && s_client != nullptr; }

bool websocket_send_audio(const uint8_t *data, size_t length)
{
    if (!data || length != 640U || !websocket_is_connected() || !websocket_setup_complete() ||
        !websocket_gemini_greeting_finished()) return false;
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
    if (s_resume_attempted) return false;
    if (!websocket_gemini_should_resume() || websocket_gemini_goaway_time_left_ms() == 0) return false;
    s_resume_attempted = true;
    return true;
}

uint64_t websocket_goaway_time_left_ms(void) { return websocket_gemini_goaway_time_left_ms(); }
