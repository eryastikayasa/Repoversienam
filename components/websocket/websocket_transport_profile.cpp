#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_transport.h"
#include "esp_tls.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "esp_aec.h"
#include <stdint.h>

namespace {
static volatile uint64_t s_poll_write_us = 0;
static volatile uint64_t s_tls_write_us = 0;
static volatile uint64_t s_transport_write_us = 0;
static volatile uint64_t s_capture_read_us = 0;
static volatile uint64_t s_i2s_read_us = 0;
static volatile uint64_t s_aec_us = 0;
static volatile uint64_t s_queue_push_us = 0;
static volatile uint64_t s_capture_count = 0;
static volatile uint64_t s_capture_read_max_us = 0;
static volatile uint64_t s_i2s_read_max_us = 0;
static volatile uint64_t s_aec_max_us = 0;
static volatile uint64_t s_queue_push_max_us = 0;
static inline bool is_task(const char *expected)
{
    const char *name = pcTaskGetName(nullptr);
    if (!name || !expected) return false;
    while (*name && *expected && *name == *expected) { ++name; ++expected; }
    return *name == '\0' && *expected == '\0';
}
static inline bool is_mic_net_task(void)
{
    const char *name = pcTaskGetName(nullptr);
    return name && name[0] == 'm' && name[1] == 'i' && name[2] == 'c' && name[3] == '_' &&
           name[4] == 'n' && name[5] == 'e' && name[6] == 't' && name[7] == '_' && name[8] == 't' && name[9] == 'x' && name[10] == '\0';
}
static inline bool is_mic_transport_task(void) { return is_task("mic_tx") || is_mic_net_task(); }
static inline void add_us(volatile uint64_t *dst, uint64_t value) { __atomic_fetch_add(dst, value, __ATOMIC_RELAXED); }
static inline uint64_t load_us(volatile uint64_t *src) { return __atomic_load_n(src, __ATOMIC_RELAXED); }
static inline void update_max(volatile uint64_t *dst, uint64_t value)
{
    uint64_t old = __atomic_load_n(dst, __ATOMIC_RELAXED);
    while (old < value && !__atomic_compare_exchange_n(dst, &old, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void log_capture_window_if_ready()
{
    const uint64_t count = load_us(&s_capture_count);
    if (count < 250U) return;
    ESP_LOGI("AUDIO_HAL", "AUDIO_CAPTURE_PROFILE: count=%llu mic_read_avg_us=%llu mic_read_max_us=%llu i2s_read_avg_us=%llu i2s_read_max_us=%llu aec_avg_us=%llu aec_max_us=%llu queue_push_avg_us=%llu queue_push_max_us=%llu",
             (unsigned long long)count,
             (unsigned long long)(load_us(&s_capture_read_us) / count), (unsigned long long)load_us(&s_capture_read_max_us),
             (unsigned long long)(load_us(&s_i2s_read_us) / count), (unsigned long long)load_us(&s_i2s_read_max_us),
             (unsigned long long)(load_us(&s_aec_us) / count), (unsigned long long)load_us(&s_aec_max_us),
             (unsigned long long)(load_us(&s_queue_push_us) / count), (unsigned long long)load_us(&s_queue_push_max_us));
    __atomic_store_n(&s_capture_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_capture_read_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_capture_read_max_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_i2s_read_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_i2s_read_max_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_aec_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_aec_max_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_queue_push_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_queue_push_max_us, 0, __ATOMIC_RELAXED);
}
}

extern "C" int __real_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms);
extern "C" int __real_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms);
extern "C" ssize_t __real_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);
extern "C" size_t __real_audio_read_mic(uint8_t *dest, size_t max_len);
extern "C" esp_err_t __real_i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);
extern "C" void __real_aec_process(aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out);
extern "C" BaseType_t __real_xQueueGenericSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait, BaseType_t copy_position);

extern "C" int __wrap_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    if (!is_mic_transport_task()) return __real_esp_transport_poll_write(t, timeout_ms);
    const int64_t start = esp_timer_get_time(); const int ret = __real_esp_transport_poll_write(t, timeout_ms);
    add_us(&s_poll_write_us, (uint64_t)(esp_timer_get_time() - start)); return ret;
}
extern "C" int __wrap_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    if (!is_mic_transport_task()) return __real_esp_transport_write(t, buffer, len, timeout_ms);
    const int64_t start = esp_timer_get_time(); const int ret = __real_esp_transport_write(t, buffer, len, timeout_ms);
    add_us(&s_transport_write_us, (uint64_t)(esp_timer_get_time() - start)); return ret;
}
extern "C" ssize_t __wrap_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen)
{
    if (!is_mic_transport_task()) return __real_esp_tls_conn_write(tls, data, datalen);
    const int64_t start = esp_timer_get_time(); const ssize_t ret = __real_esp_tls_conn_write(tls, data, datalen);
    add_us(&s_tls_write_us, (uint64_t)(esp_timer_get_time() - start)); return ret;
}
extern "C" size_t __wrap_audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!is_task("audio_capture")) return __real_audio_read_mic(dest, max_len);
    const int64_t start = esp_timer_get_time(); const size_t ret = __real_audio_read_mic(dest, max_len);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start);
    add_us(&s_capture_read_us, elapsed); __atomic_fetch_add(&s_capture_count, 1, __ATOMIC_RELAXED); update_max(&s_capture_read_max_us, elapsed); log_capture_window_if_ready(); return ret;
}
extern "C" esp_err_t __wrap_i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!is_task("audio_capture")) return __real_i2s_channel_read(handle, dest, size, bytes_read, timeout_ms);
    const int64_t start = esp_timer_get_time(); const esp_err_t ret = __real_i2s_channel_read(handle, dest, size, bytes_read, timeout_ms);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start); add_us(&s_i2s_read_us, elapsed); update_max(&s_i2s_read_max_us, elapsed); return ret;
}
extern "C" void __wrap_aec_process(aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out)
{
    if (!is_task("audio_capture")) { __real_aec_process(handle, mic, ref, out); return; }
    const int64_t start = esp_timer_get_time(); __real_aec_process(handle, mic, ref, out);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start); add_us(&s_aec_us, elapsed); update_max(&s_aec_max_us, elapsed);
}
extern "C" BaseType_t __wrap_xQueueGenericSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait, BaseType_t copy_position)
{
    if (!is_task("audio_capture")) return __real_xQueueGenericSend(queue, item, ticks_to_wait, copy_position);
    const int64_t start = esp_timer_get_time(); const BaseType_t ret = __real_xQueueGenericSend(queue, item, ticks_to_wait, copy_position);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start); add_us(&s_queue_push_us, elapsed); update_max(&s_queue_push_max_us, elapsed); return ret;
}
extern "C" void websocket_transport_profile_snapshot(uint64_t *poll_us, uint64_t *tls_us, uint64_t *transport_us)
{
    if (poll_us) *poll_us = load_us(&s_poll_write_us);
    if (tls_us) *tls_us = load_us(&s_tls_write_us);
    if (transport_us) *transport_us = load_us(&s_transport_write_us);
}
