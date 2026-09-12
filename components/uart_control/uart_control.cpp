#include "uart_control.h"
#include "driver/uart.h"
#include "hal/gpio_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "UART_CTRL";

#define UART_CONTROL_NUM      UART_NUM_1
#define UART_CONTROL_TX_PIN   GPIO_NUM_17
#define UART_CONTROL_RX_PIN   GPIO_NUM_18
#define UART_CONTROL_BAUDRATE 115200
#define UART_CONTROL_BUF_SIZE 1024
#define SENSOR_RESPONSE_MAX_LEN 32

static char last_sensor_response[SENSOR_RESPONSE_MAX_LEN];

static bool is_sensor_command(const char *command)
{
    return command && (strcmp(command, "cek_suhu") == 0 || strcmp(command, "cek_cahaya") == 0);
}

static bool is_valid_action(const char *action)
{
    static const char *const allowed[] = {
        "r1", "r2", "r3", "r4",
        "fan_pwr", "fan_speed", "fan_swing", "fan_mode",
        "mp3_mode", "mp3_play", "mp3_eq",
        "m_led", "m_mute", "m_musik", "m_cek",
        "cek_suhu", "cek_cahaya"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (strcmp(action, allowed[i]) == 0) return true;
    }
    return false;
}

void uart_control_init(void)
{
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = UART_CONTROL_BAUDRATE;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(UART_CONTROL_NUM, UART_CONTROL_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_CONTROL_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_CONTROL_NUM, UART_CONTROL_TX_PIN, UART_CONTROL_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    last_sensor_response[0] = '\0';
    ESP_LOGI(TAG, "UART control siap: TX=%d RX=%d baud=%d",
             UART_CONTROL_TX_PIN, UART_CONTROL_RX_PIN, UART_CONTROL_BAUDRATE);
}

void uart_control_send(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') return;
    uart_write_bytes(UART_CONTROL_NUM, cmd, strlen(cmd));
    uart_write_bytes(UART_CONTROL_NUM, "\n", 1);
    ESP_LOGI(TAG, "TX: %s", cmd);
}

bool uart_control_execute_sensor_command(const char *command, char *response, size_t response_len, uint32_t timeout_ms)
{
    if (response && response_len > 0) response[0] = '\0';
    if (!is_sensor_command(command) || !response || response_len < 2) return false;

    uart_flush_input(UART_CONTROL_NUM);
    uart_control_send(command);

    size_t idx = 0;
    uint8_t byte = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (idx < response_len - 1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;

        TickType_t remaining = deadline - now;
        int n = uart_read_bytes(UART_CONTROL_NUM, &byte, 1, remaining);
        if (n <= 0) continue;
        if (byte == '\r') continue;

        if (byte == '\n') {
            response[idx] = '\0';
            if (idx == 0) continue;

            const char *prefix = strcmp(command, "cek_suhu") == 0 ? "SUHU:" : "CAHAYA:";
            size_t prefix_len = strlen(prefix);
            if (strncmp(response, prefix, prefix_len) != 0) {
                ESP_LOGW(TAG, "Respons sensor tidak sesuai: %s", response);
                idx = 0;
                continue;
            }

            const char *value = response + prefix_len;
            bool numeric = (*value != '\0');
            for (const char *p = value; numeric && *p; ++p) {
                if (*p < '0' || *p > '9') numeric = false;
            }
            if (!numeric) {
                ESP_LOGW(TAG, "Nilai sensor tidak numerik: %s", response);
                idx = 0;
                continue;
            }

            ESP_LOGI(TAG, "Sensor response: %s", response);
            return true;
        }

        response[idx++] = (char)byte;
    }

    response[idx] = '\0';
    ESP_LOGW(TAG, "Timeout menunggu respons sensor: %s", command);
    return false;
}

bool uart_control_execute_command(const char *command)
{
    if (command == NULL || command[0] == '\0') return false;
    if (!is_valid_action(command)) {
        ESP_LOGW(TAG, "Command ditolak: %s", command);
        return false;
    }

    last_sensor_response[0] = '\0';

    if (is_sensor_command(command)) {
        if (!uart_control_execute_sensor_command(command, last_sensor_response,
                                                 sizeof(last_sensor_response), 1500)) {
            last_sensor_response[0] = '\0';
            return false;
        }
        ESP_LOGI(TAG, "Gemini sensor command berhasil: %s -> %s", command, last_sensor_response);
        return true;
    }

    uart_control_send(command);
    ESP_LOGI(TAG, "Gemini command diterima: %s", command);
    return true;
}

const char *uart_control_take_last_sensor_response(void)
{
    static char result[SENSOR_RESPONSE_MAX_LEN];
    if (last_sensor_response[0] == '\0') return NULL;
    strlcpy(result, last_sensor_response, sizeof(result));
    last_sensor_response[0] = '\0';
    return result;
}

bool uart_control_process_action_text(const char *text)
{
    if (text == NULL) return false;

    const char *tag = strstr(text, "[ACTION:");
    if (tag == NULL) return false;
    tag += 8;

    const char *end = strchr(tag, ']');
    if (end == NULL) return false;

    size_t len = (size_t)(end - tag);
    if (len == 0 || len >= 32) return false;

    char action[32];
    memcpy(action, tag, len);
    action[len] = '\0';
    return uart_control_execute_command(action);
}

int uart_control_read(char *buf, size_t max_len)
{
    if (buf == NULL || max_len == 0) return -1;

    size_t idx = 0;
    uint8_t byte;
    while (idx < max_len - 1) {
        int n = uart_read_bytes(UART_CONTROL_NUM, &byte, 1, 0);
        if (n <= 0) break;
        if (byte == '\r') continue;
        if (byte == '\n') {
            buf[idx] = '\0';
            return (int)idx;
        }
        buf[idx++] = (char)byte;
    }

    buf[idx] = '\0';
    return idx > 0 ? (int)idx : -1;
}
