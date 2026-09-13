#include "websocket_event.h"
#include "websocket_transport.h"
#include "websocket_audio.h"
#include "gemini_protocol.h"
#include "audio_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static constexpr size_t RX_MAX_PAYLOAD = 64U * 1024U;
static constexpr size_t RX_BUFFER_COUNT = 3U;
static constexpr uint32_t RX_WORKER_STACK = 8192U;
static constexpr UBaseType_t RX_WORKER_PRIORITY = 5U;

struct rx_item_t { char *buffer; size_t len; uint32_t generation; };
static QueueHandle_t s_rx_free_queue = nullptr;
static QueueHandle_t s_rx_ready_queue = nullptr;
static StaticQueue_t s_rx_free_queue_storage;
static StaticQueue_t s_rx_ready_queue_storage;
static char *s_rx_free_storage[RX_BUFFER_COUNT];
static rx_item_t s_rx_ready_storage[RX_BUFFER_COUNT];
static char *s_rx_buffers[RX_BUFFER_COUNT] = {};
static char *s_rx_assembling_buffer = nullptr;
static size_t s_rx_expected = 0;
static size_t s_rx_received = 0;
static uint8_t s_rx_first_opcode = 0;
static uint32_t s_rx_generation = 0;
static bool s_rx_assembling = false;
static TaskHandle_t s_rx_worker_task = nullptr;
static bool s_rx_worker_ready = false;

static bool ensure_rx_worker(void)
{
    if (s_rx_worker_ready) return true;
    s_rx_free_queue = xQueueCreateStatic(RX_BUFFER_COUNT, sizeof(char *), reinterpret_cast<uint8_t *>(s_rx_free_storage), &s_rx_free_queue_storage);
    s_rx_ready_queue = xQueueCreateStatic(RX_BUFFER_COUNT, sizeof(rx_item_t), reinterpret_cast<uint8_t *>(s_rx_ready_storage), &s_rx_ready_queue_storage);
    if (!s_rx_free_queue || !s_rx_ready_queue) { ESP_LOGE(TAG, "WS_RX: gagal membuat RX queue"); return false; }
    for (size_t i = 0; i < RX_BUFFER_COUNT; ++i) {
        s_rx_buffers[i] = static_cast<char *>(heap_caps_malloc(RX_MAX_PAYLOAD + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rx_buffers[i]) { ESP_LOGE(TAG, "WS_RX: gagal alokasi PSRAM buffer #%u", (unsigned)i); return false; }
        if (xQueueSend(s_rx_free_queue, &s_rx_buffers[i], 0) != pdPASS) return false;
    }
    if (xTaskCreate([](void *) {
            ESP_LOGI(TAG, "WS_RX: worker START buffers=%u size=%uKB", (unsigned)RX_BUFFER_COUNT, (unsigned)(RX_MAX_PAYLOAD / 1024U));
            for (;;) {
                rx_item_t item = {};
                if (xQueueReceive(s_rx_ready_queue, &item, portMAX_DELAY) != pdPASS) continue;
                if (!item.buffer || item.len == 0) continue;
                const uint32_t current_generation = websocket_transport_generation();
                if (item.generation != current_generation) {
                    ESP_LOGW(TAG, "WS_RX: stale message generation=%lu current=%lu dropped", (unsigned long)item.generation, (unsigned long)current_generation);
                } else {
                    ESP_LOGI(TAG, "WS_RX: complete message len=%u generation=%lu", (unsigned)item.len, (unsigned long)item.generation);
                    (void)gemini_protocol_process_message(item.buffer, item.len, item.generation);
                }
                if (xQueueSend(s_rx_free_queue, &item.buffer, portMAX_DELAY) != pdPASS) ESP_LOGE(TAG, "WS_RX: gagal mengembalikan RX buffer");
            }
        }, "ws_rx", RX_WORKER_STACK, nullptr, RX_WORKER_PRIORITY, &s_rx_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "WS_RX: gagal membuat RX worker"); return false;
    }
    s_rx_worker_ready = true;
    return true;
}

static void reset_rx(void)
{
    if (s_rx_assembling_buffer) {
        if (!s_rx_free_queue || xQueueSend(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) ESP_LOGW(TAG, "WS_RX: assembly buffer gagal dikembalikan");
        s_rx_assembling_buffer = nullptr;
    }
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_first_opcode = 0;
    s_rx_generation = 0;
    s_rx_assembling = false;
}

static void handle_data_event(esp_websocket_event_data_t *event)
{
    if (!event || !event->data_ptr || event->data_len <= 0 || event->payload_len <= 0) return;
    const uint8_t raw_opcode = event->op_code;
    const uint8_t opcode = raw_opcode & 0x0FU;
    const size_t payload_len = (size_t)event->payload_len;
    const size_t payload_offset = (size_t)event->payload_offset;
    const size_t data_len = (size_t)event->data_len;

    // IMPORTANT: this callback executes synchronously on websocket_task.
    // Do not perform per-chunk UART logging or other blocking work here.
    if (payload_len > RX_MAX_PAYLOAD || payload_offset > payload_len || data_len > payload_len - payload_offset) {
        ESP_LOGW(TAG, "WS_RX: invalid boundary total=%u offset=%u len=%u", (unsigned)payload_len, (unsigned)payload_offset, (unsigned)data_len);
        reset_rx(); return;
    }
    if (opcode == 0x8U || opcode == 0x9U || opcode == 0xAU) return;
    if (opcode != 0x00U && opcode != 0x01U && opcode != 0x02U) {
        ESP_LOGW(TAG, "WS_RX: unsupported opcode=0x%02X dropped", opcode);
        reset_rx(); return;
    }
    if (!ensure_rx_worker()) { ESP_LOGW(TAG, "WS_RX: RX worker unavailable; payload dropped"); reset_rx(); return; }

    if (payload_offset == 0U) {
        reset_rx();
        if (opcode != 0x01U && opcode != 0x02U) { ESP_LOGW(TAG, "WS_RX: first fragment must be TEXT/BINARY, opcode=0x%02X", opcode); return; }
        if (xQueueReceive(s_rx_free_queue, &s_rx_assembling_buffer, 0) != pdPASS) { ESP_LOGW(TAG, "WS_RX: free queue empty; payload dropped"); return; }
        s_rx_expected = payload_len;
        s_rx_received = 0;
        s_rx_first_opcode = opcode;
        s_rx_generation = websocket_transport_generation();
        s_rx_assembling = true;
    } else {
        const bool opcode_matches_message = (opcode == 0x00U || opcode == s_rx_first_opcode);
        if (!s_rx_assembling || s_rx_expected != payload_len || payload_offset != s_rx_received || !opcode_matches_message) {
            ESP_LOGW(TAG, "WS_RX: fragment sequence invalid opcode=0x%02X first_opcode=0x%02X offset=%u received=%u expected=%u total=%u", opcode, s_rx_first_opcode, (unsigned)payload_offset, (unsigned)s_rx_received, (unsigned)s_rx_expected, (unsigned)payload_len);
            reset_rx(); return;
        }
    }

    memcpy(s_rx_assembling_buffer + payload_offset, event->data_ptr, data_len);
    s_rx_received = payload_offset + data_len;

    // FIN is not authoritative here: this driver can report FIN=1 on partial chunks.
    if (s_rx_received != s_rx_expected) return;

    s_rx_assembling_buffer[s_rx_expected] = '\0';
    rx_item_t item = {s_rx_assembling_buffer, s_rx_expected, s_rx_generation};
    s_rx_assembling_buffer = nullptr;
    s_rx_expected = 0;
    s_rx_received = 0;
    s_rx_first_opcode = 0;
    s_rx_generation = 0;
    s_rx_assembling = false;
    if (xQueueSend(s_rx_ready_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "WS_RX: ready queue penuh; payload dropped");
        if (xQueueSend(s_rx_free_queue, &item.buffer, 0) != pdPASS) ESP_LOGE(TAG, "WS_RX: RX buffer hilang");
    }
}

void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        websocket_transport_event_connected(); websocket_event_reset(); (void)ensure_rx_worker();
        audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED, websocket_transport_generation());
        if (!gemini_protocol_on_connected()) { ESP_LOGE(TAG, "Gemini setup send failed"); (void)websocket_transport_disconnect(); }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (websocket_transport_is_connected()) handle_data_event(static_cast<esp_websocket_event_data_t *>(event_data));
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        websocket_audio_on_disconnected(); audio_engine_stop_input_session(); gemini_protocol_on_disconnected(); reset_rx(); websocket_transport_event_disconnected(); break;
    case WEBSOCKET_EVENT_ERROR: ESP_LOGE(TAG, "WebSocket ERROR"); break;
    default: break;
    }
}

void websocket_event_reset(void) { reset_rx(); }
bool websocket_event_gemini_ready(void) { return gemini_protocol_setup_complete(); }
