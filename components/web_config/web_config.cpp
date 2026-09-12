#include "web_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include <string.h>

namespace {
constexpr const char *NS = "config";
constexpr const char *SSID = "wifi_ssid";
constexpr const char *PASS = "wifi_pass";
constexpr const char *KEY = "api_key";
constexpr const char *ROLE = "role_text";
constexpr const char *FORCE = "force_config";

bool get_string(const char *key, char *out, size_t len) {
    if (!out || !len) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    esp_err_t e = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    return e == ESP_OK && out[0] != 0;
}

void put_string(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value ? value : "");
    nvs_commit(h);
    nvs_close(h);
}
}

extern "C" bool web_config_api_key_is_valid(const char *api_key) {
    if (!api_key) return false;
    size_t n = strlen(api_key);
    if (n < 20 || n >= 128) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)api_key[i];
        if (c <= 0x20 || c >= 0x7f) return false;
    }
    return true;
}

extern "C" bool web_config_is_needed(void) {
    char ssid[64] = {};
    char key[128] = {};
    char force[8] = {};
    bool have_ssid = get_string(SSID, ssid, sizeof(ssid));
    bool have_key = get_string(KEY, key, sizeof(key));
    bool forced = get_string(FORCE, force, sizeof(force)) && strcmp(force, "1") == 0;
    return forced || !have_ssid || !have_key || !web_config_api_key_is_valid(key);
}

extern "C" void web_config_start(void) {}

extern "C" void web_config_save(const char *wifi_ssid, const char *wifi_pass, const char *api_key, const char *role_text) {
    put_string(SSID, wifi_ssid);
    put_string(PASS, wifi_pass);
    put_string(KEY, api_key);
    put_string(ROLE, role_text);
    put_string(FORCE, "0");
}

extern "C" bool web_config_load_role(char *buf, size_t max_len) { return get_string(ROLE, buf, max_len); }

extern "C" bool web_config_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    if (!get_string(SSID, ssid, ssid_len)) return false;
    get_string(PASS, pass, pass_len);
    return true;
}

extern "C" bool web_config_load_api_key(char *api_key, size_t max_len) { return get_string(KEY, api_key, max_len); }

extern "C" void web_config_force_reset(void) {
    put_string(FORCE, "1");
    esp_restart();
}
