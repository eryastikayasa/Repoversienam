#include "web_config.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_attr.h"

static const char *TAG = "WEB_CONFIG";

#define CONFIG_NAMESPACE "config"
#define KEY_WIFI_SSID    "wifi_ssid"
#define KEY_WIFI_PASS    "wifi_pass"
#define KEY_API_KEY      "api_key"
#define KEY_ROLE_TEXT    "role_text"
#define KEY_FORCE_CONFIG "force_config"

#define CONFIG_BOOT_BUTTON_GPIO GPIO_NUM_0
#define CONFIG_LONG_PRESS_MS 2000

static const char *HTML_FORM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Config</title>
<style>
  body { font-family: sans-serif; background: #1e1e24; color: #f0f0f5; padding: 20px; }
  input, textarea { width: 100%; padding: 10px; margin: 6px 0; border-radius: 6px; border: 1px solid #333; background: #121216; color: #fff; box-sizing: border-box; }
  button { width: 100%; padding: 12px; background: #4338ca; color: white; border: none; border-radius: 6px; font-weight: bold; }
</style>
</head>
<body>
<h2>Konfigurasi Asisten</h2>
<form action="/save" method="POST">
  <label>SSID WiFi</label>
  <input type="text" name="wifi_ssid" required>
  <label>Password WiFi</label>
  <input type="password" name="wifi_pass">
  <label>API Key Gemini</label>
  <input type="password" name="api_key" required>
  <label>Ingatan / Kepribadian AI</label>
  <textarea name="role_text" rows="6" placeholder="Nama saya Bima, suka kopi pahit..."></textarea>
  <button type="submit">Simpan & Restart</button>
</form>
</body>
</html>
)rawliteral";

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    if (!dst || !src || dst_len == 0) return;
    size_t src_len = strlen(src);
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '%' && i + 2 < src_len) {
            unsigned int h = 0;
            if (sscanf(src + i + 1, "%2x", &h) == 1) {
                dst[j++] = (char)h;
                i += 3;
                continue;
            }
        }
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
            continue;
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

static bool nvs_get_str_safe(const char *key, char *out, size_t max_len)
{
    if (!key || !out || max_len == 0) return false;
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = max_len;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    return err == ESP_OK && out[0] != '\0';
}

static void nvs_set_str_safe(const char *key, const char *value)
{
    if (!key || !value) return;
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, key, value);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void log_api_key_diag(const char *api_key, const char *source)
{
    if (!api_key) {
        ESP_LOGW(TAG, "API key diagnostic [%s]: NULL", source ? source : "?");
        return;
    }

    const size_t len = strlen(api_key);
    char prefix[5] = "";
    char suffix[5] = "";
    if (len >= 4) {
        memcpy(prefix, api_key, 4);
        memcpy(suffix, api_key + len - 4, 4);
        prefix[4] = '\0';
        suffix[4] = '\0';
    } else {
        strncpy(prefix, api_key, sizeof(prefix) - 1);
    }

    ESP_LOGI(TAG, "API key diagnostic [%s]: len=%u prefix=%s suffix=%s valid=%s",
             source ? source : "?",
             (unsigned)len,
             prefix,
             len >= 4 ? suffix : "-",
             web_config_api_key_is_valid(api_key) ? "YES" : "NO");
}

static bool api_key_contains_forbidden_text(const char *api_key)
{
    if (!api_key) return true;
    return strstr(api_key, "#include") != NULL ||
           strstr(api_key, "Arduino.h") != NULL ||
           strstr(api_key, "#include <") != NULL;
}

extern "C" bool web_config_api_key_is_valid(const char *api_key)
{
    if (!api_key) return false;

    const size_t len = strlen(api_key);
    if (len < 20 || len >= 128) return false;
    if (api_key_contains_forbidden_text(api_key)) return false;

    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)api_key[i];
        if (c <= 0x20 || c == 0x7F ||
            c == '<' || c == '>' ||
            c == '"' || c == '\'' ||
            c == '\\' || c == '/' ||
            c == '&' || c == '#') {
            return false;
        }
    }

    return true;
}

static void dump_config_nvs_keys(void)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", CONFIG_NAMESPACE, NVS_TYPE_ANY, &it);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS namespace '%s': tidak ada entry yang dapat didaftarkan",
                 CONFIG_NAMESPACE);
        return;
    }

    ESP_LOGI(TAG, "NVS namespace '%s' key inventory:", CONFIG_NAMESPACE);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        ESP_LOGI(TAG, "  key=%s type=%d", info.key, (int)info.type);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

static bool boot_button_long_pressed(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_BOOT_BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    if (gpio_get_level(CONFIG_BOOT_BUTTON_GPIO) != 0) {
        return false;
    }

    ESP_LOGI(TAG, "GPIO0 ditekan saat boot, cek long-press %d ms...", CONFIG_LONG_PRESS_MS);
    const int step_ms = 50;
    int held_ms = 0;

    while (gpio_get_level(CONFIG_BOOT_BUTTON_GPIO) == 0 && held_ms < CONFIG_LONG_PRESS_MS) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        held_ms += step_ms;
    }

    if (held_ms >= CONFIG_LONG_PRESS_MS && gpio_get_level(CONFIG_BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGW(TAG, "GPIO0 long-press terdeteksi -> masuk Config Mode");
        return true;
    }

    ESP_LOGI(TAG, "GPIO0 dilepas sebelum long-press -> lanjut boot normal");
    return false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_FORM, strlen(HTML_FORM));
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    static EXT_RAM_BSS_ATTR char content[2048];
    int total = 0;
    while (total < (int)sizeof(content) - 1) {
        int received = httpd_req_recv(req, content + total, sizeof(content) - 1 - total);
        if (received <= 0) break;
        total += received;
    }
    content[total] = '\0';

    char wifi_ssid[64] = "";
    char wifi_pass[64] = "";
    char api_key[128] = "";
    char role_text[512] = "";

    char *saveptr = NULL;
    char *token = strtok_r(content, "&", &saveptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *key = token;
            const char *value = eq + 1;
            char decoded[512] = "";
            url_decode(decoded, sizeof(decoded), value);

            if (strcmp(key, "wifi_ssid") == 0) {
                strncpy(wifi_ssid, decoded, sizeof(wifi_ssid) - 1);
            } else if (strcmp(key, "wifi_pass") == 0) {
                strncpy(wifi_pass, decoded, sizeof(wifi_pass) - 1);
            } else if (strcmp(key, "api_key") == 0) {
                strncpy(api_key, decoded, sizeof(api_key) - 1);
            } else if (strcmp(key, "role_text") == 0) {
                strncpy(role_text, decoded, sizeof(role_text) - 1);
            }
        }
        token = strtok_r(NULL, "&", &saveptr);
    }

    if (wifi_ssid[0] == '\0' || api_key[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "SSID WiFi dan API Key wajib diisi.");
        return ESP_OK;
    }

    log_api_key_diag(api_key, "POST sebelum NVS");
    if (!web_config_api_key_is_valid(api_key)) {
        ESP_LOGE(TAG, "API key ditolak: format tidak valid atau terdeteksi sebagai data/source code lain");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "API Key tidak valid. Periksa kembali kolom API Key Gemini.");
        return ESP_OK;
    }

    web_config_save(wifi_ssid, wifi_pass, api_key, role_text);
    ESP_LOGI(TAG, "Konfigurasi WiFi/API key/role berhasil disimpan, restart...");

    httpd_resp_sendstr(req, "OK. Konfigurasi tersimpan, perangkat restart...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

extern "C" {

bool web_config_is_needed(void)
{
    if (boot_button_long_pressed()) {
        return true;
    }

    nvs_handle_t handle;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return true;

    char ssid[64] = "";
    char api_key[128] = "";
    char force[8] = "";
    size_t len;

    len = sizeof(ssid);
    esp_err_t ssid_err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &len);
    len = sizeof(api_key);
    esp_err_t key_err = nvs_get_str(handle, KEY_API_KEY, api_key, &len);
    len = sizeof(force);
    esp_err_t force_err = nvs_get_str(handle, KEY_FORCE_CONFIG, force, &len);
    nvs_close(handle);

    if (force_err == ESP_OK && strcmp(force, "1") == 0) return true;
    return ssid_err != ESP_OK || ssid[0] == '\0' || key_err != ESP_OK || !web_config_api_key_is_valid(api_key);
}

void web_config_start(void)
{
    ESP_LOGI(TAG, "Memulai mode konfigurasi AP");

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "nvs_flash_init gagal: %s", esp_err_to_name(err));
        return;
    }

    dump_config_nvs_keys();

    char stored_api_key[128] = "";
    if (nvs_get_str_safe(KEY_API_KEY, stored_api_key, sizeof(stored_api_key))) {
        log_api_key_diag(stored_api_key, "NVS saat Config Mode start");
    } else {
        ESP_LOGI(TAG, "API key NVS belum tersedia saat Config Mode start");
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default gagal: %s", esp_err_to_name(err));
        return;
    }

    if (!esp_netif_create_default_wifi_ap()) {
        ESP_LOGE(TAG, "Gagal membuat WiFi AP netif");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init gagal: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {};
    strcpy((char *)ap_config.ap.ssid, "ESP32-Config");
    ap_config.ap.ssid_len = strlen("ESP32-Config");
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.server_port = 80;
    server_cfg.stack_size = 12288;
    server_cfg.task_priority = 6;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_cfg) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &save_uri);
        ESP_LOGI(TAG, "Web config server berjalan di http://192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Gagal start HTTP server");
    }
}

void web_config_save(const char *wifi_ssid,
                     const char *wifi_pass,
                     const char *api_key,
                     const char *role_text)
{
    nvs_set_str_safe(KEY_WIFI_SSID, wifi_ssid ? wifi_ssid : "");
    nvs_set_str_safe(KEY_WIFI_PASS, wifi_pass ? wifi_pass : "");
    nvs_set_str_safe(KEY_API_KEY, api_key ? api_key : "");
    nvs_set_str_safe(KEY_ROLE_TEXT, role_text ? role_text : "");
    nvs_set_str_safe(KEY_FORCE_CONFIG, "0");
}

bool web_config_load_role(char *buf, size_t max_len)
{
    return nvs_get_str_safe(KEY_ROLE_TEXT, buf, max_len);
}

bool web_config_load_wifi(char *ssid, size_t ssid_len,
                          char *pass, size_t pass_len)
{
    if (!ssid || !pass || ssid_len == 0 || pass_len == 0) return false;
    if (!nvs_get_str_safe(KEY_WIFI_SSID, ssid, ssid_len)) return false;
    nvs_get_str_safe(KEY_WIFI_PASS, pass, pass_len);
    return true;
}

bool web_config_load_api_key(char *api_key, size_t max_len)
{
    return nvs_get_str_safe(KEY_API_KEY, api_key, max_len);
}

void web_config_force_reset(void)
{
    nvs_set_str_safe(KEY_FORCE_CONFIG, "1");
    esp_restart();
}

} // extern "C"
