#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Repo6 WebSocket public boundary.
 *
 * WebSocket is transport-only:
 *   Audio Engine -> WebSocket -> Gemini
 *   Gemini -> WebSocket -> Audio Engine
 *
 * Audio format, playback, I2S, and Gemini protocol details stay outside
 * this public transport API.
 */

/** Initialize the WebSocket transport layer. */
void websocket_init(void);

/** Connect the transport to the configured server. */
bool websocket_connect(void);

/** Disconnect the transport. */
void websocket_disconnect(void);

/** Return true when the transport is connected. */
bool websocket_is_connected(void);

/** Send an audio payload owned by the caller. */
bool websocket_send_audio(const uint8_t *data, size_t length);

/** Send a text/control payload owned by the caller. */
bool websocket_send_text(const char *text);

#ifdef __cplusplus
}
#endif
