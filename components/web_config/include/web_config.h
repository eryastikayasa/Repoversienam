#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
bool web_config_is_needed(void);
void web_config_start(void);
void web_config_save(const char *wifi_ssid, const char *wifi_pass, const char *api_key, const char *role_text);
bool web_config_load_role(char *buf, size_t max_len);
bool web_config_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
bool web_config_load_api_key(char *api_key, size_t max_len);
bool web_config_api_key_is_valid(const char *api_key);
void web_config_force_reset(void);
#ifdef __cplusplus
}
#endif
