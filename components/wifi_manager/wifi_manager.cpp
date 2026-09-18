#include "wifi_manager.h"
#include "web_config.h"

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_event_group = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_STARTED_BIT   BIT1
#define WIFI_FAILED_BIT    BIT2

static volatile bool s_wifi_started = false;
static volatile bool s_wifi_got_ip = false;

static size_t dns_skip_name(const uint8_t *packet, size_t len, size_t offset)
{
    if (!packet || offset >= len) return 0;
    size_t pos = offset;
    size_t guard = 0;
    while (pos < len && guard++ < 128) {
        uint8_t c = packet[pos];
        if (c == 0) return (pos + 1) - offset;
        if ((c & 0xC0) == 0xC0) return (pos + 2 <= len) ? (pos + 2) - offset : 0;
        if ((c & 0xC0) != 0 || c > 63) return 0;
        if (pos + 1 + c > len) return 0;
        pos += 1 + c;
    }
    return 0;
}

static bool direct_dns_query(const char *dns_ip, const char *host, const char *label)
{
    if (!dns_ip || !host) return false;

    uint8_t packet[512] = {};
    uint16_t txid = 0x5A17;
    packet[0] = (uint8_t)(txid >> 8);
    packet[1] = (uint8_t)(txid & 0xFF);
    packet[2] = 0x01;
    packet[5] = 0x01;

    size_t pos = 12;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t part_len = dot ? (size_t)(dot - p) : strlen(p);
        if (part_len == 0 || part_len > 63 || pos + 1 + part_len >= sizeof(packet)) return false;
        packet[pos++] = (uint8_t)part_len;
        memcpy(&packet[pos], p, part_len);
        pos += part_len;
        if (!dot) break;
        p = dot + 1;
    }
    packet[pos++] = 0;
    packet[pos++] = 0x00;
    packet[pos++] = 0x01;
    packet[pos++] = 0x00;
    packet[pos++] = 0x01;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DIRECT DNS [%s] socket() gagal errno=%d", label, errno);
        return false;
    }

    struct timeval timeout = {};
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server = {};
    server.sin_family = AF_INET;
    server.sin_port = htons(53);
    if (inet_pton(AF_INET, dns_ip, &server.sin_addr) != 1) {
        ESP_LOGE(TAG, "DIRECT DNS [%s] DNS IP tidak valid: %s", label, dns_ip);
        close(sock);
        return false;
    }

    int64_t start_us = esp_timer_get_time();
    ssize_t sent = sendto(sock, packet, pos, 0, (struct sockaddr *)&server, sizeof(server));
    if (sent < 0) {
        ESP_LOGE(TAG, "DIRECT DNS [%s] sendto(%s:53) gagal errno=%d", label, dns_ip, errno);
        close(sock);
        return false;
    }

    uint8_t response[512] = {};
    struct sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    ssize_t received = recvfrom(sock, response, sizeof(response), 0, (struct sockaddr *)&from, &from_len);
    int saved_errno = errno;
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    close(sock);

    if (received < 12) {
        ESP_LOGE(TAG, "DIRECT DNS [%s] server=%s recv=%d errno=%d elapsed=%lld ms", label, dns_ip, (int)received, saved_errno, (long long)(elapsed_us / 1000));
        return false;
    }

    uint16_t response_txid = ((uint16_t)response[0] << 8) | response[1];
    uint16_t flags = ((uint16_t)response[2] << 8) | response[3];
    uint16_t answers = ((uint16_t)response[6] << 8) | response[7];
    uint8_t rcode = (uint8_t)(flags & 0x0F);
    bool qr = (flags & 0x8000) != 0;

    ESP_LOGI(TAG, "DIRECT DNS [%s] server=%s bytes=%d txid=0x%04X rcode=%u answers=%u elapsed=%lld ms",
             label, dns_ip, (int)received, response_txid, (unsigned)rcode, (unsigned)answers, (long long)(elapsed_us / 1000));

    if (response_txid != txid || !qr || rcode != 0 || answers == 0) {
        ESP_LOGE(TAG, "DIRECT DNS [%s] RESULT=FAILED", label);
        return false;
    }

    size_t answer_pos = 12;
    uint16_t questions = ((uint16_t)response[4] << 8) | response[5];
    for (uint16_t i = 0; i < questions; ++i) {
        size_t skipped = dns_skip_name(response, (size_t)received, answer_pos);
        if (skipped == 0 || answer_pos + skipped + 4 > (size_t)received) {
            ESP_LOGE(TAG, "DIRECT DNS [%s] malformed question", label);
            return false;
        }
        answer_pos += skipped + 4;
    }

    for (uint16_t i = 0; i < answers && answer_pos + 12 <= (size_t)received; ++i) {
        size_t skipped = dns_skip_name(response, (size_t)received, answer_pos);
        if (skipped == 0 || answer_pos + skipped + 10 > (size_t)received) break;
        answer_pos += skipped;
        uint16_t type = ((uint16_t)response[answer_pos] << 8) | response[answer_pos + 1];
        uint16_t rdlength = ((uint16_t)response[answer_pos + 8] << 8) | response[answer_pos + 9];
        answer_pos += 10;
        if (answer_pos + rdlength > (size_t)received) break;
        if (type == 1 && rdlength == 4) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &response[answer_pos], ip, sizeof(ip));
            ESP_LOGI(TAG, "DIRECT DNS [%s] IPv4=%s", label, ip);
            ESP_LOGI(TAG, "DIRECT DNS [%s] RESULT=OK", label);
            return true;
        }
        answer_pos += rdlength;
    }

    ESP_LOGW(TAG, "DIRECT DNS [%s] response valid tetapi belum menemukan IPv4 A record", label);
    return true;
}

static void direct_dns_diagnostic(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DIRECT UDP DNS DIAGNOSTIC");
    ESP_LOGI(TAG, "========================================");
    const char *servers[] = { "8.8.8.8", "8.8.4.4" };
    const char *hosts[] = { "google.com", "generativelanguage.googleapis.com" };
    const char *host_labels[] = { "google.com", "Gemini" };

    for (size_t s = 0; s < sizeof(servers) / sizeof(servers[0]); ++s) {
        for (size_t h = 0; h < sizeof(hosts) / sizeof(hosts[0]); ++h) {
            direct_dns_query(servers[s], hosts[h], host_labels[h]);
        }
    }
    ESP_LOGI(TAG, "DIRECT UDP DNS DIAGNOSTIC END");
    ESP_LOGI(TAG, "========================================");
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Wi-Fi STA START");
        s_wifi_started = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGE(TAG, "esp_wifi_connect gagal: %s", esp_err_to_name(err));
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_wifi_got_ip = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi GOT_IP - network READY");

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event && event->esp_netif) {
            esp_netif_ip_info_t ip_info = {};
            if (esp_netif_get_ip_info(event->esp_netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "NET IP=" IPSTR " MASK=" IPSTR " GW=" IPSTR,
                         IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
            }

            const esp_netif_dns_type_t dns_types[] = {
                ESP_NETIF_DNS_MAIN,
                ESP_NETIF_DNS_BACKUP,
                ESP_NETIF_DNS_FALLBACK
            };
            const char *dns_names[] = { "MAIN", "BACKUP", "FALLBACK" };

            for (size_t i = 0; i < sizeof(dns_types) / sizeof(dns_types[0]); ++i) {
                esp_netif_dns_info_t dns = {};
                esp_err_t dns_err = esp_netif_get_dns_info(event->esp_netif, dns_types[i], &dns);
                if (dns_err == ESP_OK) {
                    ESP_LOGI(TAG, "DNS STA %s=" IPSTR, dns_names[i], IP2STR(&dns.ip.u_addr.ip4));
                } else {
                    ESP_LOGW(TAG, "DNS STA %s tidak tersedia: %s", dns_names[i], esp_err_to_name(dns_err));
                }

                esp_netif_dns_info_t global_dns = {};
                esp_err_t global_dns_err = esp_netif_get_dns_info(NULL, dns_types[i], &global_dns);
                if (global_dns_err == ESP_OK) {
                    ESP_LOGI(TAG, "DNS GLOBAL %s=" IPSTR, dns_names[i], IP2STR(&global_dns.ip.u_addr.ip4));
                } else {
                    ESP_LOGW(TAG, "DNS GLOBAL %s tidak tersedia: %s", dns_names[i], esp_err_to_name(global_dns_err));
                }
            }

            // TEST ONLY: force the DHCP-provided primary DNS to the known-good server.
            // The Wi-Fi IP/gateway remain unchanged; only DNS MAIN is overridden.
            esp_netif_dns_info_t forced_dns = {};
            forced_dns.ip.type = ESP_IPADDR_TYPE_V4;
            forced_dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.4.4");
            esp_err_t force_err = esp_netif_set_dns_info(
                event->esp_netif,
                ESP_NETIF_DNS_MAIN,
                &forced_dns
            );
            if (force_err == ESP_OK) {
                ESP_LOGI(TAG, "DNS TEST: MAIN dipaksa ke 8.8.4.4");
            } else {
                ESP_LOGE(TAG, "DNS TEST: gagal memaksa MAIN=8.8.4.4: %s", esp_err_to_name(force_err));
            }

            esp_netif_dns_info_t verify_dns = {};
            esp_err_t verify_err = esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &verify_dns);
            if (verify_err == ESP_OK) {
                ESP_LOGI(TAG, "DNS TEST: MAIN setelah override=" IPSTR,
                         IP2STR(&verify_dns.ip.u_addr.ip4));
            }
        }

        direct_dns_diagnostic();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_wifi_got_ip = false;
        if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi TERPUTUS");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Reconnect Wi-Fi gagal: %s", esp_err_to_name(err));
        return;
    }
}

void wifi_init_sta(void)
{
    ESP_LOGI(TAG, "Memulai Wi-Fi manager V7.0.4");

    if (s_wifi_event_group != NULL) {
        ESP_LOGW(TAG, "Wi-Fi manager sudah diinisialisasi");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Gagal membuat Wi-Fi event group");
        return;
    }

    s_wifi_started = false;
    s_wifi_got_ip = false;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default gagal: %s", esp_err_to_name(err));
        return;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "Gagal membuat default Wi-Fi STA netif");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register WIFI_EVENT gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register IP_EVENT gagal: %s", esp_err_to_name(err));
        return;
    }

    char ssid[64] = "";
    char pass[64] = "";
    if (!web_config_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGE(TAG, "WiFi SSID tidak ditemukan di NVS");
        return;
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config gagal: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start gagal: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi driver STARTED");
}

bool wifi_wait_for_connection(uint32_t timeout_ms)
{
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "WAIT GOT_IP gagal: event group NULL");
        return false;
    }

    if (s_wifi_got_ip) {
        ESP_LOGI(TAG, "Wi-Fi sudah READY");
        return true;
    }

    ESP_LOGI(TAG, "Menunggu WIFI GOT_IP...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms)
    );

    if ((bits & WIFI_CONNECTED_BIT) != 0 && s_wifi_got_ip) {
        ESP_LOGI(TAG, "Wi-Fi READY - GOT_IP diterima");
        return true;
    }

    ESP_LOGE(TAG, "Timeout menunggu WIFI GOT_IP");
    return false;
}

bool wifi_is_ready(void)
{
    return (s_wifi_started && s_wifi_got_ip);
}
