#pragma once

#include "esp_err.h"
#include "esp_websocket_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t websocket_transport_init(void);
esp_err_t websocket_transport_connect(void);
esp_err_t websocket_transport_disconnect(void);
esp_err_t websocket_transport_abort(void);
bool websocket_transport_is_connected(void);
esp_websocket_client_handle_t websocket_transport_client(void);
uint32_t websocket_transport_generation(void);
esp_err_t websocket_transport_send_text(const char *text, size_t len);
void websocket_transport_profile_snapshot(uint64_t *poll_write_us,
                                          uint64_t *tls_write_us,
                                          uint64_t *transport_write_us);
void websocket_transport_event_connected(void);
void websocket_transport_event_disconnected(void);

#ifdef __cplusplus
}
#endif
