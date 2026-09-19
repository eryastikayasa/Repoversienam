#include "display_engine.h"
#include "display_face.h"
#include "display_text.h"
#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "websocket_internal.h"
#include "uart_control.h"
#include "web_config.h"
#include "wakeword.h"
#include "afe_audio.h"

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
static void oled_init(void)
{
    display_engine_init();
    display_engine_start();
}

static void face_animation_start(void)
{
    display_engine_start();
}

static void face_set_state(face_state_t state)
{
    display_face_set_state(state);
}

static void display_status(const char *text)
{
    display_text_set_status(text ? text : "");
}


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
static volatile bool audio_task_reading = false;
/* True while the current Gemini session owns a live AFE instance. */
static volatile bool afe_session_active = false;

static void wait_for_gemini_mic_release(void)
{
    (void)audio_hal_stop_capture();
    for (uint32_t i = 0; i < 100 && audio_task_reading; ++i)
        vTaskDelay(pdMS_TO_TICKS(1));
    if (audio_task_reading)
        ESP_LOGW(TAG, "MIC handoff: audio_task masih membaca setelah 100 ms");
}

/*
 * A Gemini session must never leak into the next WakeWord cycle.
 * After a session ends, wait for the websocket client cleanup worker to
 * finish destroying the old client before accepting the next HI, ESP.
 * This makes session 2/3/... follow the same fresh lifecycle as boot.
 */
static bool wait_for_websocket_idle(uint32_t timeout_ms)
{
    const uint32_t step_ms = 10;
    const uint32_t max_steps = timeout_ms / step_ms;

    for (uint32_t i = 0; i < max_steps; ++i) {
        if (client == NULL)
            return true;
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    ESP_LOGW(TAG, "WebSocket lifecycle belum idle setelah %u ms", (unsigned)timeout_ms);
    return client == NULL;
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t raw_pcm[512];
    static int16_t afe_input_staging[1024];
    static int16_t afe_pcm[512];
    static uint8_t audio_buffer[16384];
    size_t buffer_pos = 0;
    size_t afe_staging_samples = 0;
    int64_t last_afe_log_us = 0;
    uint32_t afe_input_frames = 0;
    uint32_t afe_output_frames = 0;
    uint64_t afe_input_samples = 0;
    uint64_t afe_output_samples = 0;
    uint64_t afe_output_bytes = 0;
    uint32_t afe_tx_frames = 0;
    int64_t last_afe_flow_log_us = 0;

    while (1) {
        if (!assistant_active) {
            /* Gemini owns AFE only for an active session. Deinitialize it
             * from audio_task itself so no other task can destroy the AFE
             * while this task is still feeding/flushing it. */
            if (afe_session_active) {
                ESP_LOGI(TAG, "AFE SESSION: stopping Gemini AFE at session boundary");
                afe_audio_deinit();
                afe_session_active = false;
                ESP_LOGI(TAG, "AFE SESSION: Gemini AFE deinitialized");
            }
            buffer_pos = 0;
            afe_staging_samples = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        audio_task_reading = true;
        size_t bytes_read = audio_read_mic(
            reinterpret_cast<uint8_t *>(raw_pcm),
            sizeof(raw_pcm));
        audio_task_reading = false;

        if (bytes_read > 0) {
            const size_t samples_read = bytes_read / sizeof(int16_t);

            /* Audio HAL already supplies PCM16/16 kHz/mono. AFE's feed chunk is
             * determined at runtime by get_feed_chunksize(); HAL reads up to
             * 512 samples and staging handles any remainder.
             * Stage the existing PCM and feed exact runtime AFE chunks reported by
             * get_feed_chunksize(), without changing the audio format or Audio HAL. */
            if (mic_frame_has_activity(reinterpret_cast<const uint8_t *>(raw_pcm), bytes_read)) {
                last_user_activity_us = esp_timer_get_time();
            }

            if (afe_audio_is_ready()) {
                const size_t feed_samples = (size_t)afe_audio_get_feed_samples();

                if (feed_samples > 0 && feed_samples <= sizeof(afe_input_staging) / sizeof(afe_input_staging[0])) {
                    if (afe_staging_samples + samples_read <= sizeof(afe_input_staging) / sizeof(afe_input_staging[0])) {
                        memcpy(afe_input_staging + afe_staging_samples,
                               raw_pcm,
                               samples_read * sizeof(int16_t));
                        afe_staging_samples += samples_read;
                    } else {
                        ESP_LOGW(TAG, "AFE staging overflow; resetting pending PCM");
                        afe_staging_samples = 0;
                    }

                    while (afe_staging_samples >= feed_samples) {
                        size_t ignored_output_samples = 0;
                        if (!afe_audio_process(afe_input_staging, feed_samples,
                                               afe_pcm, sizeof(afe_pcm) / sizeof(afe_pcm[0]),
                                               &ignored_output_samples)) {
                            ESP_LOGW(TAG, "AFE process gagal untuk feed=%u",
                                     (unsigned)feed_samples);
                            break;
                        }

                        ++afe_input_frames;
                        afe_input_samples += feed_samples;

                        /*
                         * AFE feed/fetch are asynchronous. Drain every processed
                         * frame currently available instead of consuming only one
                         * result per feed. The external Gemini contract stays
                         * identical to the pre-AFE path: accumulate PCM16 until
                         * 3200 bytes, then call websocket_send_audio_data().
                         */
                        for (;;) {
                            size_t afe_samples = 0;
                            if (!afe_audio_fetch_output(afe_pcm,
                                                        sizeof(afe_pcm) / sizeof(afe_pcm[0]),
                                                        &afe_samples)) {
                                ESP_LOGW(TAG, "AFE output fetch gagal");
                                break;
                            }
                            if (afe_samples == 0) break;

                            const size_t afe_bytes = afe_samples * sizeof(int16_t);
                            ++afe_output_frames;
                            afe_output_samples += afe_samples;
                            afe_output_bytes += afe_bytes;

                            if (buffer_pos + afe_bytes > sizeof(audio_buffer)) {
                                ESP_LOGE(TAG, "AFE output buffer overflow: pending=%u incoming=%u",
                                         (unsigned)buffer_pos, (unsigned)afe_bytes);
                                break;
                            }

                            memcpy(audio_buffer + buffer_pos, afe_pcm, afe_bytes);
                            buffer_pos += afe_bytes;
                        }

                        const size_t remainder = afe_staging_samples - feed_samples;
                        if (remainder > 0) {
                            memmove(afe_input_staging,
                                    afe_input_staging + feed_samples,
                                    remainder * sizeof(int16_t));
                        }
                        afe_staging_samples = remainder;
                    }
                }
            } else {
                int64_t now_log = esp_timer_get_time();
                if (last_afe_log_us == 0 || now_log - last_afe_log_us >= 2000000) {
                    last_afe_log_us = now_log;
                    ESP_LOGW(TAG, "AFE Gemini path not ready; raw audio is not sent");
                }
            }
        }

        if (!websocket_is_connected()) {
            if (esp_timer_get_time() - connect_start_us > 15 * 1000000LL) {
                ESP_LOGW(TAG, "Koneksi gagal. Kembali ke mode sleep.");
                assistant_active = false;
                (void)audio_hal_stop_capture();
                face_set_state(FACE_SLEEP);
                buffer_pos = 0;
                continue;
            }
            /* Keep a bounded AFE pre-roll while the WebSocket is connecting.
             * This prevents the first part of the user's sentence from being
             * erased merely because TLS/WebSocket setup is still in progress. */
            if (buffer_pos > sizeof(audio_buffer)) buffer_pos = sizeof(audio_buffer);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (buffer_pos >= 3200) {
            const int64_t now_us = esp_timer_get_time();

            if (now_us - last_user_activity_us > 60 * 1000000LL) {
                ESP_LOGI(TAG, "Idle 60 detik, menutup sesi.");
                assistant_active = false;
                (void)audio_hal_stop_capture();
                face_set_state(FACE_SLEEP);
                websocket_disconnect();
                buffer_pos = 0;
                continue;
            }

            /*
             * Do not perform amplitude-VAD gating here. Every processed AFE
             * frame is valid Gemini input. Gemini Live Automatic Activity
             * Detection remains responsible for speech turn boundaries.
             */
            if (!audio_turn_active) {
                websocket_send_audio_data(audio_buffer, 3200);
                ++afe_tx_frames;
            } else {
                ESP_LOGD(TAG, "AFE FLOW: TX ditahan karena Gemini sedang playback (turn aktif)");
            }

            const size_t remainder = buffer_pos - 3200;
            if (remainder > 0) memmove(audio_buffer, audio_buffer + 3200, remainder);
            buffer_pos = remainder;
        }

        const int64_t flow_now_us = esp_timer_get_time();
        if (last_afe_flow_log_us == 0 || flow_now_us - last_afe_flow_log_us >= 2000000) {
            last_afe_flow_log_us = flow_now_us;
            ESP_LOGI(TAG,
                     "AFE FLOW: input_frames=%u input_samples=%llu output_frames=%u output_samples=%llu output_bytes=%llu tx_100ms_frames=%u buffer=%u turn_active=%d",
                     (unsigned)afe_input_frames,
                     (unsigned long long)afe_input_samples,
                     (unsigned)afe_output_frames,
                     (unsigned long long)afe_output_samples,
                     (unsigned long long)afe_output_bytes,
                     (unsigned)afe_tx_frames,
                     (unsigned)buffer_pos,
                     audio_turn_active ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
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
        if (websocket_standby_requested()) {
            /* STANDBY-ONLY PATH. Normal Gemini turns never enter this
             * shutdown sequence. standby_gemini only marks shutdown pending;
             * Gemini must be allowed to produce its final spoken response. */
            const bool response_started = websocket_standby_response_started();
            const bool response_drained = response_started &&
                                          !audio_turn_active &&
                                          !audio_turn_complete_pending;

            if (assistant_active && response_drained) {
                ESP_LOGI(TAG, "Standby Gemini: final response audio drained -> shutdown");

                /* Stop microphone ownership first; audio_task then performs
                 * the AFE deinit at its own session boundary. */
                assistant_active = false;
                wait_for_gemini_mic_release();

                /* Do not start WakeWord/new Gemini until the old AFE fetch
                 * task and instance are completely gone. */
                while (afe_session_active)
                    vTaskDelay(pdMS_TO_TICKS(1));

                /*
                 * Return to the exact boot-ready lifecycle:
                 * Gemini AFE is already deinitialized by audio_task above,
                 * then close the old websocket and wait until its client is
                 * actually destroyed before the next WakeWord cycle can
                 * accept a new session.
                 */
                websocket_clear_standby_request();
                websocket_disconnect();
                (void)wait_for_websocket_idle(3000);
                /* End-of-session transcript reset. The locked renderer keeps
                 * its buffers across sessions; clear them at the lifecycle
                 * boundary so session N+1 starts with a clean text layer. */
                display_text_set_user("");
                display_text_set_gemini("");
                face_set_state(FACE_SLEEP);
                display_status("Katakan: Hi, ESP");
                last_user_activity_us = 0;
                connect_start_us = 0;
                ESP_LOGI(TAG, "Standby Gemini: old session fully released -> boot-ready lifecycle");
            }
        }

        if (!assistant_active) {
            if (wakeword_detected()) {
                /*
                 * Do not consume HI, ESP while the previous websocket client
                 * is still being destroyed. Boot has no old client; every
                 * subsequent session must wait for the same clean boundary.
                 */
                if (client != NULL) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                wakeword_clear_detected();
                wakeword_stop();
                (void)audio_hal_stop_capture();
                if (!afe_audio_init()) {
                    ESP_LOGE(TAG, "AFE init gagal; Gemini session tidak dimulai");
                    face_set_state(FACE_ERROR);
                    continue;
                }
                afe_session_active = true;

                /* Fresh session: discard transcript from the previous
                 * Gemini conversation before the new WakeWord handoff. */
                display_text_set_user("");
                display_text_set_gemini("");

                assistant_active = true;
                connect_start_us = esp_timer_get_time();
                last_user_activity_us = connect_start_us;
                face_set_state(FACE_HAPPY);
                ESP_LOGI(TAG, "WAKEWORD detected -> conversation handoff");
                websocket_app_start();
            } else if (client == NULL && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    wakeword_stop();
                    (void)audio_hal_stop_capture();
                    wakeword_clear_detected();
                    ESP_LOGI(TAG, "Tombol ditekan! Memulai sesi...");
                    if (!afe_audio_init()) {
                        ESP_LOGE(TAG, "AFE init gagal; Gemini session tidak dimulai");
                        face_set_state(FACE_ERROR);
                        continue;
                    }
                    afe_session_active = true;

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
