#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void websocket_audio_init(void);
bool websocket_audio_send_frame(const uint8_t *data, size_t len);
void websocket_audio_on_disconnected(void);

#ifdef __cplusplus
}
#endif
