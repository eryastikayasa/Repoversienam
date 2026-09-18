#pragma once
#include "websocket_mgr.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SESSION_HANDLE_MAX_LEN 1024
#define WS_RX_MAX_PAYLOAD_SIZE (48 * 1024)
#define WS_RX_SLOT_SIZE (40 * 1024)
/* v7.0.31: increase persistent RX slot headroom after v7.0.30 showed
 * buffer_drop=4 and queue_hwm=4 during Gemini audio bursts. */
#define WS_RX_SLOT_COUNT 8
#define WS_RX_TERMINATOR_SIZE 1
#define WS_RX_STREAM_THRESHOLD (8 * 1024)
#define WS_RX_STREAM_COMPACT_SIZE (8 * 1024)
#define WS_TX_AUDIO_SIZE 3200
#define WS_TX_QUEUE_LENGTH 3
#define WS_RX_QUEUE_LENGTH WS_RX_SLOT_COUNT

typedef enum { WS_TX_COMMAND_SETUP = 1, WS_TX_COMMAND_AUDIO = 2 } ws_tx_command_type_t;
typedef struct { ws_tx_command_type_t type; uint32_t generation; uint16_t len; uint8_t *data; } ws_tx_command_t;
extern QueueHandle_t websocket_tx_queue; extern TaskHandle_t websocket_tx_task_handle;
bool websocket_tx_init(void); bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation); void websocket_tx_flush_queue(void);
typedef struct { uint32_t generation; uint8_t *buffer; uint32_t len; uint8_t slot_id; } ws_rx_command_t;
extern QueueHandle_t websocket_rx_queue; extern TaskHandle_t websocket_rx_task_handle;
bool websocket_rx_init(void); void websocket_rx_request_reset(void); void websocket_rx_flush_queue(void); bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data, uint32_t generation); void websocket_rx_note_invalid_json(size_t len);
extern esp_websocket_client_handle_t client; extern volatile bool is_connected; extern volatile bool setup_complete; extern volatile bool websocket_tx_error; extern char session_handle[SESSION_HANDLE_MAX_LEN]; extern bool session_resumable;
extern StreamBufferHandle_t audio_stream; extern TaskHandle_t audio_playback_task_handle; extern volatile bool audio_turn_active; extern volatile bool audio_turn_complete_pending; extern volatile uint32_t websocket_connection_generation;
void websocket_schedule_setup(uint32_t generation);
extern uint32_t audio_chunks_received; extern uint64_t audio_bytes_received; extern uint64_t audio_bytes_queued; extern uint32_t audio_write_calls; extern uint64_t audio_bytes_played; extern uint64_t audio_bytes_dropped;
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data); void reset_rx_buffer(void); bool ensure_rx_buffer(size_t required_size); void process_websocket_payload(esp_websocket_event_data_t *data); void process_gemini_message(const char *json, size_t len); bool build_gemini_setup(char **output, size_t *output_len); void clear_session_handle(void); bool store_session_handle(const char *handle); size_t get_audio_pending_bytes(void); bool start_audio_playback(void); void clear_audio_buffer(void); void request_audio_buffer_clear(void); void reset_audio_turn_stats(void); void begin_audio_turn(void); bool queue_audio_pcm(const uint8_t *pcm, size_t len); void check_audio_playback_complete(void);
void websocket_disconnect(void);
void websocket_reset_started(void);
bool websocket_cleanup_is_pending(void);
void websocket_cleanup_complete(void);
bool websocket_send_tool_response(const char *id, const char *name, bool success);
bool websocket_send_tool_response_text(const char *id, const char *name, const char *result);
