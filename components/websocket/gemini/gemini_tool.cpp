#include "gemini_tool.h"
#include "uart_control.h"
#include "websocket_transport.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "GEMINI_TOOL";
static constexpr size_t TOOL_QUEUE_LEN = 4U;
static constexpr size_t TOOL_ID_MAX = 128U;
static constexpr size_t TOOL_NAME_MAX = 64U;
static constexpr size_t TOOL_COMMAND_MAX = 32U;
static constexpr uint32_t TOOL_WORKER_STACK = 4096U;
static constexpr UBaseType_t TOOL_WORKER_PRIORITY = 4U;

struct tool_job_t {
    char id[TOOL_ID_MAX];
    char name[TOOL_NAME_MAX];
    char command[TOOL_COMMAND_MAX];
};

static QueueHandle_t s_tool_queue = nullptr;
static StaticQueue_t s_tool_queue_storage;
static uint8_t s_tool_queue_buffer[TOOL_QUEUE_LEN * sizeof(tool_job_t)];
static TaskHandle_t s_tool_worker = nullptr;

extern "C" esp_err_t __real_websocket_transport_send_text(const char *text, size_t len);

static void add_device_control_tool(cJSON *setup)
{
    if (!cJSON_IsObject(setup)) return;

    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    if (!tools) return;
    cJSON *tool = cJSON_CreateObject();
    cJSON *functions = cJSON_CreateArray();
    cJSON *decl = cJSON_CreateObject();
    if (!tool || !functions || !decl) {
        cJSON_Delete(tool);
        cJSON_Delete(functions);
        cJSON_Delete(decl);
        return;
    }

    cJSON_AddItemToObject(tool, "functionDeclarations", functions);
    cJSON_AddStringToObject(decl, "name", "control_device");
    cJSON_AddStringToObject(decl, "description",
        "Mengontrol perangkat rumah melalui UART. Gunakan hanya untuk aksi perangkat dan tunggu hasil fungsi sebelum menjawab pengguna.");

    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *command = cJSON_CreateObject();
    cJSON *enum_values = cJSON_CreateArray();
    cJSON *required = cJSON_CreateArray();
    if (!parameters || !properties || !command || !enum_values || !required) {
        cJSON_Delete(tool);
        cJSON_Delete(decl);
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(command);
        cJSON_Delete(enum_values);
        cJSON_Delete(required);
        return;
    }

    cJSON_AddStringToObject(parameters, "type", "OBJECT");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(properties, "command", command);
    cJSON_AddStringToObject(command, "type", "STRING");
    cJSON_AddStringToObject(command, "description", "Satu command UART yang didukung.");

    static const char *const commands[] = {
        "r1", "r2", "r3", "r4",
        "fan_pwr", "fan_speed", "fan_swing", "fan_mode",
        "mp3_mode", "mp3_play", "mp3_eq",
        "m_led", "m_mute", "m_musik", "m_cek",
        "cek_suhu", "cek_cahaya"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        cJSON_AddItemToArray(enum_values, cJSON_CreateString(commands[i]));
    cJSON_AddItemToObject(command, "enum", enum_values);
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddItemToObject(decl, "parameters", parameters);
    cJSON_AddItemToArray(functions, decl);
    cJSON_AddItemToArray(tools, tool);
}

extern "C" esp_err_t __wrap_websocket_transport_send_text(const char *text, size_t len)
{
    if (!text || len == 0) return __real_websocket_transport_send_text(text, len);

    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return __real_websocket_transport_send_text(text, len);

    cJSON *setup = cJSON_GetObjectItemCaseSensitive(root, "setup");
    if (!cJSON_IsObject(setup) || cJSON_GetObjectItemCaseSensitive(setup, "tools")) {
        cJSON_Delete(root);
        return __real_websocket_transport_send_text(text, len);
    }

    add_device_control_tool(setup);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return __real_websocket_transport_send_text(text, len);

    const size_t payload_len = strlen(payload);
    esp_err_t err = __real_websocket_transport_send_text(payload, payload_len);
    cJSON_free(payload);
    return err;
}

static bool ensure_tool_worker(void)
{
    if (s_tool_worker) return true;

    s_tool_queue = xQueueCreateStatic(
        TOOL_QUEUE_LEN,
        sizeof(tool_job_t),
        s_tool_queue_buffer,
        &s_tool_queue_storage);
    if (!s_tool_queue) {
        ESP_LOGE(TAG, "Gagal membuat tool queue");
        return false;
    }

    if (xTaskCreate(
            [](void *) {
                ESP_LOGI(TAG, "UART tool worker START");
                for (;;) {
                    tool_job_t job = {};
                    if (xQueueReceive(s_tool_queue, &job, portMAX_DELAY) != pdPASS) continue;

                    bool success = false;
                    const char *result = nullptr;
                    char sensor_result[64] = {0};

                    if (strcmp(job.name, "control_device") == 0) {
                        success = uart_control_execute_command(job.command);
                        if (success) {
                            const char *stored = uart_control_take_last_sensor_response();
                            if (stored && stored[0]) {
                                strlcpy(sensor_result, stored, sizeof(sensor_result));
                                result = sensor_result;
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "Tool tidak didukung: %s", job.name);
                    }

                    cJSON *root = cJSON_CreateObject();
                    cJSON *tool_response = cJSON_CreateObject();
                    cJSON *responses = cJSON_CreateArray();
                    cJSON *function_response = cJSON_CreateObject();
                    cJSON *response = cJSON_CreateObject();
                    bool ok = root && tool_response && responses && function_response && response;

                    if (ok) ok = cJSON_AddItemToObject(root, "toolResponse", tool_response);
                    if (ok) ok = cJSON_AddItemToObject(tool_response, "functionResponses", responses);
                    if (ok) ok = cJSON_AddItemToArray(responses, function_response);
                    if (ok) ok = cJSON_AddStringToObject(function_response, "id", job.id) != nullptr;
                    if (ok) ok = cJSON_AddStringToObject(function_response, "name", job.name) != nullptr;
                    if (ok) ok = cJSON_AddItemToObject(function_response, "response", response);
                    if (ok) {
                        const char *final_result = result ? result : (success ? "ok" : "error");
                        ok = cJSON_AddStringToObject(response, "result", final_result) != nullptr;
                    }

                    if (ok) {
                        char *payload = cJSON_PrintUnformatted(root);
                        if (payload) {
                            const size_t len = strlen(payload);
                            if (websocket_transport_send_text(payload, len) != ESP_OK)
                                ESP_LOGW(TAG, "toolResponse gagal dikirim id=%s", job.id);
                            else
                                ESP_LOGI(TAG, "toolResponse terkirim id=%s command=%s result=%s",
                                         job.id, job.command, result ? result : (success ? "ok" : "error"));
                            cJSON_free(payload);
                        }
                    }
                    cJSON_Delete(root);
                }
            },
            "gemini_tool",
            TOOL_WORKER_STACK,
            nullptr,
            TOOL_WORKER_PRIORITY,
            &s_tool_worker) != pdPASS) {
        s_tool_worker = nullptr;
        ESP_LOGE(TAG, "Gagal membuat UART tool worker");
        return false;
    }
    return true;
}

extern "C" void gemini_tool_handle_call(const cJSON *tool_call)
{
    if (!cJSON_IsObject(tool_call)) return;
    if (!ensure_tool_worker()) return;

    cJSON *function_calls = cJSON_GetObjectItemCaseSensitive(tool_call, "functionCalls");
    if (!cJSON_IsArray(function_calls)) {
        ESP_LOGW(TAG, "toolCall.functionCalls bukan array");
        return;
    }

    cJSON *fc = nullptr;
    cJSON_ArrayForEach(fc, function_calls) {
        if (!cJSON_IsObject(fc)) continue;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(fc, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(fc, "name");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
        cJSON *command = cJSON_IsObject(args)
            ? cJSON_GetObjectItemCaseSensitive(args, "command") : nullptr;

        if (!cJSON_IsString(id) || !id->valuestring ||
            !cJSON_IsString(name) || !name->valuestring ||
            !cJSON_IsObject(args) ||
            !cJSON_IsString(command) || !command->valuestring) {
            ESP_LOGW(TAG, "toolCall function tidak lengkap; diabaikan");
            continue;
        }

        if (strlen(id->valuestring) >= TOOL_ID_MAX ||
            strlen(name->valuestring) >= TOOL_NAME_MAX ||
            strlen(command->valuestring) >= TOOL_COMMAND_MAX) {
            ESP_LOGW(TAG, "toolCall field terlalu panjang");
            continue;
        }

        tool_job_t job = {};
        strlcpy(job.id, id->valuestring, sizeof(job.id));
        strlcpy(job.name, name->valuestring, sizeof(job.name));
        strlcpy(job.command, command->valuestring, sizeof(job.command));

        if (xQueueSend(s_tool_queue, &job, 0) != pdPASS)
            ESP_LOGW(TAG, "Tool queue penuh; function id=%s diabaikan", job.id);
        else
            ESP_LOGI(TAG, "Tool queued: name=%s id=%s command=%s", job.name, job.id, job.command);
    }
}
