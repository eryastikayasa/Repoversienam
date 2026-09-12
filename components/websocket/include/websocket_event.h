#pragma once

#include "esp_websocket_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void websocket_event_reset(void);
bool websocket_event_gemini_ready(void);

#ifdef __cplusplus
}
#endif
