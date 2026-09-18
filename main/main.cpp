#include "display.h"
#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "websocket_internal.h"
#include "uart_control.h"
#include "web_config.h"
#include "wakeword.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_sntp.h"

#include <sys/time.h>
#include <time.h>

#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *TAG = "MAIN";
#define BOOT_BUTTON_GPIO GPIO_NUM_0

static bool resolve_host(const char *label, const char *host, const char *port, char *resolved_ip, size_t resolved_ip_len)
{
    ESP_LOGI(TAG, "DNS [%s]: getaddrinfo(%s:%s)", label, host, port);
    struct addrinfo hints = {};
    struct addrinfo *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int saved_errno = 0;
    int64_t start_us = esp_timer_get_time();
    int err = getaddrinfo(host, port, &hints, &result);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    saved_errno = errno;
    ESP_LOGI(TAG, "DNS [%s]: err=%d errno=%d elapsed=%lld ms", label, err, saved_errno, (long long)(elapsed_us / 1000));
    if (err != 0 || result == nullptr) {
        ESP_LOGE(TAG, "DNS [%s]: FAILED", label);
        return false;
    }

    bool found_ipv4 = false;
    if (resolved_ip && resolved_ip_len > 0) resolved_ip[0] = '\0';
    for (struct addrinfo *p = result; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        struct sockaddr_in *addr = (struct sockaddr_in *)p->ai_addr;
        char ip[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != nullptr) {
            ESP_LOGI(TAG, "DNS [%s]: IPv4=%s", label, ip);
            if (!found_ipv4 && resolved_ip && resolved_ip_len > 0) {
                strlcpy(resolved_ip, ip, resolved_ip_len);
            }
        }
        found_ipv4 = true;
    }
    freeaddrinfo(result);
    if (!found_ipv4) {
        ESP_LOGE(TAG, "DNS [%s]: OK tetapi tidak ada IPv4", label);
        return false;
    }
    ESP_LOGI(TAG, "DNS [%s]: RESULT=OK", label);
    return true;
}

static bool debug_dns_server(const char *label, const char *dns_ip)
{
    if (!dns_ip || dns_ip[0] == '\0' || strcmp(dns_ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "DNS SERVER [%s]: tidak dikonfigurasi", label);
        return false;
    }
    ESP_LOGI(TAG, "DNS SERVER [%s]: %s", label, dns_ip);
    return resolve_host(label, dns_ip, "53", nullptr, 0);
}

static bool debug_dns_resolution(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DEBUG NETWORK START");

    char google_ip[INET_ADDRSTRLEN] = {};
    char gemini_ip[INET_ADDRSTRLEN] = {};

    bool google_ok = resolve_host("google.com", "google.com", "443", google_ip, sizeof(google_ip));
    bool gemini_ok = resolve_host("Gemini", "generativelanguage.googleapis.com", "443", gemini_ip, sizeof(gemini_ip));

    ESP_LOGI(TAG, "DNS SUMMARY: google.com=%s Gemini=%s", google_ok ? "OK" : "FAILED", gemini_ok ? "OK" : "FAILED");
    if (!google_ok || !gemini_ok) {
        ESP_LOGI(TAG, "========================================");
        return false;
    }
    ESP_LOGI(TAG, "DNS RESULT: OK");
    ESP_LOGI(TAG, "Gemini resolved IP: %s", gemini_ip);
    ESP_LOGI(TAG, "========================================");
    return true;
}

static bool debug_tcp_connection(void)
{
    char gemini_ip[INET_ADDRSTRLEN] = {};
    if (!resolve_host("Gemini-TCP", "generativelanguage.googleapis.com", "443", gemini_ip, sizeof(gemini_ip))) {
        ESP_LOGE(TAG, "TCP test dihentikan: DNS Gemini gagal");
        return false;
    }

    ESP_LOGI(TAG, "TCP test: generativelanguage.googleapis.com:443 -> %s:443", gemini_ip);
    struct sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(443);
    if (inet_pton(AF_INET, gemini_ip, &target.sin_addr) != 1) {
        ESP_LOGE(TAG, "TCP target IP tidak valid: %s", gemini_ip);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "TCP socket() FAILED errno=%d", errno);
        return false;
    }
    struct timeval timeout = {};
    timeout.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "TCP connect() ke %s:443...", gemini_ip);
    int64_t start_us = esp_timer_get_time();
    int ret = connect(sock, (struct sockaddr *)&target, sizeof(target));
    int saved_errno = errno;
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (ret == 0) {
        ESP_LOGI(TAG, "TCP CONNECT OK elapsed=%lld ms", (long long)(elapsed_us / 1000));
        close(sock);
        ESP_LOGI(TAG, "TCP RESULT: OK");
        return true;
    }
    ESP_LOGE(TAG, "TCP CONNECT FAILED errno=%d elapsed=%lld ms", saved_errno, (long long)(elapsed_us / 1000));
    close(sock);
    ESP_LOGE(TAG, "TCP RESULT: GAGAL");
    return false;
}

static void debug_network_path(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK DIAGNOSTIC");
    ESP_LOGI(TAG, "Target: generativelanguage.googleapis.com:443");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(TAG, "STEP 1: Network interface state sudah dilog oleh WIFI_MGR saat GOT_IP");
    ESP_LOGI(TAG, "STEP 2: DNS server reachability akan diuji melalui resolver");
    ESP_LOGI(TAG, "STEP 3: DNS google.com");
    ESP_LOGI(TAG, "STEP 4: DNS generativelanguage.googleapis.com");
    ESP_LOGI(TAG, "STEP 5: TCP 443 ke IP Gemini hasil DNS");

    if (!debug_dns_resolution()) {
        ESP_LOGE(TAG, "NETWORK STOP: DNS");
        ESP_LOGI(TAG, "========================================");
        return;
    }
    if (!debug_tcp_connection()) {
        ESP_LOGE(TAG, "NETWORK STOP: TCP");
        ESP_LOGI(TAG, "DNS = OK");
        ESP_LOGI(TAG, "TCP = FAILED");
        ESP_LOGI(TAG, "TLS = BELUM DITES");
        ESP_LOGI(TAG, "========================================");
        return;
    }
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK BASIC TEST = OK");
    ESP_LOGI(TAG, "DNS = OK");
    ESP_LOGI(TAG, "TCP 443 = OK");
    ESP_LOGI(TAG, "NEXT = WebSocket/TLS");
    ESP_LOGI(TAG, "========================================");
}

static void sync_sntp_time(void)
{
    ESP_LOGI(TAG, "Mencari server NTP..."); display_status("Sync Jam Network..");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL); esp_sntp_setservername(0, "time.google.com"); esp_sntp_setservername(1, "id.pool.ntp.org"); esp_sntp_setservername(2, "pool.ntp.org"); esp_sntp_init();
    int retry = 0; const int max_retries = 10; time_t now = 0; struct tm timeinfo = {};
    while (retry < max_retries) { time(&now); localtime_r(&now, &timeinfo); if (timeinfo.tm_year >= (2024 - 1900)) { ESP_LOGI(TAG, "Waktu cocok! Tahun: %d", timeinfo.tm_year + 1900); display_status("Jam Cocok!"); vTaskDelay(pdMS_TO_TICKS(1000)); return; } vTaskDelay(pdMS_TO_TICKS(500)); retry++; }
    ESP_LOGW(TAG, "NTP gagal. Menggunakan waktu fallback."); struct timeval tv = { .tv_sec = 1770000000, .tv_usec = 0 }; settimeofday(&tv, NULL); display_status("Jam Set Fallback"); vTaskDelay(pdMS_TO_TICKS(1000));
}

static bool mic_frame_has_activity(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return false;

    constexpr int32_t SILENCE_THRESHOLD = 500;
    constexpr size_t MIN_ACTIVE_SAMPLES = 8;
    size_t active_samples = 0;

    for (size_t i = 0; i + 1 < len; i += 2) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude >= SILENCE_THRESHOLD) {
            active_samples++;
            if (active_samples >= MIN_ACTIVE_SAMPLES) return true;
        }
    }

    return false;
}

static bool assistant_active = false;
static int64_t last_user_activity_us = 0;
static int64_t connect_start_us = 0;

static void audio_task(void *arg)
{
    (void)arg;
    static uint8_t audio_buffer[4096];
    size_t buffer_pos = 0;
    uint32_t silent_frames = 0;
    int64_t last_silent_log_us = 0;

    while (1) {
        if (!assistant_active) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytes_read = audio_read_mic(
            audio_buffer + buffer_pos,
            sizeof(audio_buffer) - buffer_pos);
        if (bytes_read > 0) buffer_pos += bytes_read;

        if (!websocket_is_connected()) {
            if (esp_timer_get_time() - connect_start_us > 15 * 1000000LL) {
                ESP_LOGW(TAG, "Koneksi gagal. Kembali ke mode sleep.");
                assistant_active = false;
                face_set_state(FACE_SLEEP);
                buffer_pos = 0;
                continue;
            }
            buffer_pos = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (buffer_pos >= 3200) {
            bool has_activity = mic_frame_has_activity(audio_buffer, 3200);
            if (has_activity) last_user_activity_us = esp_timer_get_time();
            int64_t now_us = esp_timer_get_time();

            if (now_us - last_user_activity_us > 60 * 1000000LL) {
                ESP_LOGI(TAG, "Idle 60 detik, menutup sesi.");
                assistant_active = false;
                face_set_state(FACE_SLEEP);
                websocket_disconnect();
                buffer_pos = 0;
                continue;
            }

            if (!audio_turn_active) {
                if (has_activity) {
                    websocket_send_audio_data(audio_buffer, 3200);
                } else {
                    silent_frames++;
                    int64_t now_log = esp_timer_get_time();
                    if (last_silent_log_us == 0 || now_log - last_silent_log_us >= 1000000) {
                        last_silent_log_us = now_log;
                        ESP_LOGI(TAG, "V7.0.36 MIC TX gate: silent frames dropped=%lu", (unsigned long)silent_frames);
                    }
                }
            }

            size_t remainder = buffer_pos - 3200;
            if (remainder > 0) memmove(audio_buffer, audio_buffer + 3200, remainder);
            buffer_pos = remainder;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main()
{
    ESP_LOGI("MAIN", "Total PSRAM: %d bytes", esp_psram_get_size());
    ESP_LOGI("MAIN", "Free Heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI("MAIN", "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "ESP32-S3 Asisten Kamar Dimulai...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (web_config_is_needed()) {
        oled_init();
        display_status("Config Mode");
        web_config_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    oled_init();
    face_animation_start();
    face_set_state(FACE_SLEEP);
    display_status("Booting...");

    audio_hal_init();
    audio_i2s_test_tone();

    if (!wakeword_init()) {
        ESP_LOGE(TAG, "WakeNet init gagal. Sistem tetap bisa dimulai dengan tombol BOOT.");
        display_status("WakeNet gagal!");
    } else {
        ESP_LOGI(TAG, "WAKEWORD: model=wn9_hiesp sample_rate=16000 mono DET_MODE_90");
        ESP_LOGI(TAG, "WAKEWORD: chunk_samples=%d", wakeword_get_chunk_samples());
        display_status("Katakan: Hi, ESP");
    }

    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "Tombol boot siap di GPIO0");
    uart_control_init();

    display_status("Menghubungkan WiFi...");
    wifi_init_sta();
    if (!wifi_wait_for_connection(15000)) {
        ESP_LOGE(TAG, "Wi-Fi tidak mendapatkan IP.");
        display_status("WiFi Gagal!");
        face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power save dimatikan");
    ESP_LOGI(TAG, "WIFI READY - lanjut ke NTP");
    sync_sntp_time();
    ESP_LOGI(TAG, "Menunggu 1 detik...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    debug_network_path();
    vTaskDelay(pdMS_TO_TICKS(1000));
    face_set_state(FACE_SLEEP);
    display_status("Sistem siap. Katakan Hi, ESP...");

    BaseType_t task_result = xTaskCreate(
        audio_task, "audio_task", 10240, NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat audio_task!");
    } else {
        ESP_LOGI(TAG, "audio_task berhasil dimulai.");
    }

    if (!wakeword_start()) {
        ESP_LOGE(TAG, "WAKEWORD: gagal start dedicated task");
    }

    while (1) {
        if (!assistant_active) {
            if (wakeword_detected()) {
                wakeword_clear_detected();
                wakeword_stop();
                assistant_active = true;
                connect_start_us = esp_timer_get_time();
                last_user_activity_us = connect_start_us;
                face_set_state(FACE_HAPPY);
                ESP_LOGI(TAG, "WAKEWORD detected -> conversation handoff");
                websocket_app_start();
            } else if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    wakeword_stop();
                    wakeword_clear_detected();
                    ESP_LOGI(TAG, "Tombol ditekan! Memulai sesi...");
                    assistant_active = true;
                    connect_start_us = esp_timer_get_time();
                    last_user_activity_us = connect_start_us;
                    face_set_state(FACE_HAPPY);
                    websocket_app_start();
                }
            } else if (!wakeword_is_running()) {
                (void)wakeword_start();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
