#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cek apakah perlu masuk mode konfigurasi.
bool web_config_is_needed(void);

// Mulai Access Point dan HTTP server untuk konfigurasi.
void web_config_start(void);

// Simpan konfigurasi ke NVS.
void web_config_save(const char *wifi_ssid,
                     const char *wifi_pass,
                     const char *api_key,
                     const char *role_text);

// Baca role text dari NVS.
bool web_config_load_role(char *buf, size_t max_len);

// Baca SSID dan password WiFi dari NVS.
bool web_config_load_wifi(char *ssid, size_t ssid_len,
                          char *pass, size_t pass_len);

// Baca API key Gemini dari NVS.
bool web_config_load_api_key(char *api_key, size_t max_len);

// Validasi API key tanpa mencetak nilai rahasia.
bool web_config_api_key_is_valid(const char *api_key);

// Set force_config = 1, lalu restart.
void web_config_force_reset(void);

#ifdef __cplusplus
}
#endif
