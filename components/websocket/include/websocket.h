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
 * I2S, microphone, speaker, DMA, audio buffers and audio processing remain
 * in Audio HAL/Audio Engine. Gemini session state is exposed only as small
 * transport lifecycle signals needed by app_startup.
 */
void websocket_init(void);
bool websocket_connect(void);
void websocket_disconnect(void);

/* End only the currently active Gemini session. This is not persistent
 * standby mode; the normal disconnect lifecycle will re-arm Wake Word. */
void websocket_end_session(void);

bool websocket_is_connected(void);

/* Audio payload is always PCM16/16kHz/mono/LE at this boundary. */
bool websocket_send_audio(const uint8_t *data, size_t length);
bool websocket_send_text(const char *text);

/* Session lifecycle signals. */
bool websocket_setup_complete(void);
bool websocket_should_resume(void);
bool websocket_take_resume_request(void);
uint64_t websocket_goaway_time_left_ms(void);

#ifdef __cplusplus
}
#endif
