#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
static volatile uint64_t s_capture_count = 0;
static volatile uint64_t s_capture_read_max_us = 0;
static volatile uint64_t s_i2s_read_max_us = 0;
static volatile uint64_t s_aec_max_us = 0;

static inline bool is_task(const char *expected)
{
    const char *name = pcTaskGetName(nullptr);
    if (!name || !expected) return false;
    while (*name && *expected && *name == *expected) { ++name; ++expected; }
    return *name == '\0' && *expected == '\0';
}

static inline void add_us(volatile uint64_t *dst, uint64_t value)
{
    __atomic_fetch_add(dst, value, __ATOMIC_RELAXED);
}

static inline uint64_t load_us(volatile uint64_t *src)
{
    return __atomic_load_n(src, __ATOMIC_RELAXED);
}

static inline void update_max(volatile uint64_t *dst, uint64_t value)
{
    uint64_t old = __atomic_load_n(dst, __ATOMIC_RELAXED);
    while (old < value && !__atomic_compare_exchange_n(dst, &old, value, false,
                                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

} // namespace

extern "C" int __real_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms);
extern "C" int __real_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms);
extern "C" ssize_t __real_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);
extern "C" size_t __real_audio_read_mic(uint8_t *dest, size_t max_len);
extern "C" esp_err_t __real_i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);
extern "C" void __real_aec_process(aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out);

extern "C" int __wrap_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    if (!is_task("mic_tx")) return __real_esp_transport_poll_write(t, timeout_ms);
    const int64_t start = esp_timer_get_time();
    const int ret = __real_esp_transport_poll_write(t, timeout_ms);
    add_us(&s_poll_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" int __wrap_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    if (!is_task("mic_tx")) return __real_esp_transport_write(t, buffer, len, timeout_ms);
    const int64_t start = esp_timer_get_time();
    const int ret = __real_esp_transport_write(t, buffer, len, timeout_ms);
    add_us(&s_transport_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" ssize_t __wrap_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen)
{
    if (!is_task("mic_tx")) return __real_esp_tls_conn_write(tls, data, datalen);
    const int64_t start = esp_timer_get_time();
    const ssize_t ret = __real_esp_tls_conn_write(tls, data, datalen);
    add_us(&s_tls_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" size_t __wrap_audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!is_task("audio_capture")) return __real_audio_read_mic(dest, max_len);
    const int64_t start = esp_timer_get_time();
    const size_t ret = __real_audio_read_mic(dest, max_len);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start);
    add_us(&s_capture_read_us, elapsed);
    __atomic_fetch_add(&s_capture_count, 1, __ATOMIC_RELAXED);
    update_max(&s_capture_read_max_us, elapsed);
    return ret;
}

extern "C" esp_err_t __wrap_i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!is_task("audio_capture")) return __real_i2s_channel_read(handle, dest, size, bytes_read, timeout_ms);
    const int64_t start = esp_timer_get_time();
    const esp_err_t ret = __real_i2s_channel_read(handle, dest, size, bytes_read, timeout_ms);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start);
    add_us(&s_i2s_read_us, elapsed);
    update_max(&s_i2s_read_max_us, elapsed);
    return ret;
}

extern "C" void __wrap_aec_process(aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out)
{
    if (!is_task("audio_capture")) {
        __real_aec_process(handle, mic, ref, out);
        return;
    }
    const int64_t start = esp_timer_get_time();
    __real_aec_process(handle, mic, ref, out);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start);
    add_us(&s_aec_us, elapsed);
    update_max(&s_aec_max_us, elapsed);
}

extern "C" void websocket_transport_profile_snapshot(uint64_t *poll_us, uint64_t *tls_us, uint64_t *transport_us)
{
    if (poll_us) *poll_us = load_us(&s_poll_write_us);
    if (tls_us) *tls_us = load_us(&s_tls_write_us);
    if (transport_us) *transport_us = load_us(&s_transport_write_us);
}

extern "C" void audio_capture_profile_snapshot(uint64_t *count, uint64_t *read_us,
                                                  uint64_t *read_max_us, uint64_t *i2s_us,
                                                  uint64_t *i2s_max_us, uint64_t *aec_us,
                                                  uint64_t *aec_max_us)
{
    if (count) *count = load_us(&s_capture_count);
    if (read_us) *read_us = load_us(&s_capture_read_us);
    if (read_max_us) *read_max_us = load_us(&s_capture_read_max_us);
    if (i2s_us) *i2s_us = load_us(&s_i2s_read_us);
    if (i2s_max_us) *i2s_max_us = load_us(&s_i2s_read_max_us);
    if (aec_us) *aec_us = load_us(&s_aec_us);
    if (aec_max_us) *aec_max_us = load_us(&s_aec_max_us);
}
