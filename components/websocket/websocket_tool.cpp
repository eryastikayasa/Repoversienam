#include "websocket_internal.h"
#include "uart_control.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WS_TOOL";

static bool valid_tool_id(const char *id)
{
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' )) {
            return false;
        }
    }
    return true;
}

static bool send_tool_payload(const char *id, const char *name, const char *result)
{
    if (!id || !name || !result || !client || !is_connected || websocket_tx_error) return false;
    if (!esp_websocket_client_is_connected(client)) return false;
    if (strcmp(name, "control_device") != 0) return false;
    if (!valid_tool_id(id)) {
        ESP_LOGW(TAG, "Tool call id ditolak karena karakter tidak aman");
        return false;
    }

    char payload[768];
    int len = snprintf(
        payload, sizeof(payload),
        "{\"toolResponse\":{\"functionResponses\":[{\"id\":\"%s\",\"name\":\"%s\",\"response\":{\"result\":\"%s\"}}]}}",
        id, name, result);
    if (len <= 0 || (size_t)len >= sizeof(payload)) return false;

    int sent = esp_websocket_client_send_text(client, payload, len, pdMS_TO_TICKS(5000));
    if (sent != len) {
        ESP_LOGW(TAG, "Tool response gagal dikirim: sent=%d expected=%d", sent, len);
        return false;
    }

    ESP_LOGI(TAG, "Tool response terkirim: %s id=%s result=%s", name, id, result);
    return true;
}

bool websocket_send_tool_response(const char *id, const char *name, bool success)
{
    const char *sensor_result = uart_control_take_last_sensor_response();
    if (sensor_result && sensor_result[0] != '\0') {
        return send_tool_payload(id, name, sensor_result);
    }
    return send_tool_payload(id, name, success ? "ok" : "error");
}

bool websocket_send_tool_response_text(const char *id, const char *name, const char *result)
{
    if (!result || result[0] == '\0') return false;
    return send_tool_payload(id, name, result);
}
