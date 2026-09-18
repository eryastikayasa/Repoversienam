#include "esp_websocket_client.h"

/*
 * V7.0.12
 *
 * The ESP-IDF WebSocket task can overflow its default stack while
 * bringing up WSS/TLS on ESP32-S3. Keep application audio code untouched
 * and raise only the WebSocket client task stack.
 */
#define WS_CLIENT_TASK_STACK_SAFE 12288

extern esp_websocket_client_handle_t __real_esp_websocket_client_init(
    const esp_websocket_client_config_t *config
);

esp_websocket_client_handle_t __wrap_esp_websocket_client_init(
    const esp_websocket_client_config_t *config
)
{
    if (config == NULL) {
        return __real_esp_websocket_client_init(config);
    }

    esp_websocket_client_config_t patched = *config;

    if (patched.task_stack < WS_CLIENT_TASK_STACK_SAFE) {
        patched.task_stack = WS_CLIENT_TASK_STACK_SAFE;
    }

    return __real_esp_websocket_client_init(&patched);
}
