#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_transport.h"
#include "esp_tls.h"

#include <stdint.h>

namespace {

static volatile uint64_t s_poll_write_us = 0;
static volatile uint64_t s_tls_write_us = 0;
static volatile uint64_t s_transport_write_us = 0;

static inline bool is_mic_tx_task()
{
    const char *name = pcTaskGetName(nullptr);
    return name && name[0] == 'm' && name[1] == 'i' && name[2] == 'c' &&
           name[3] == '_' && name[4] == 't' && name[5] == 'x' && name[6] == '\0';
}

static inline void add_us(volatile uint64_t *dst, uint64_t value)
{
    __atomic_fetch_add(dst, value, __ATOMIC_RELAXED);
}

static inline uint64_t load_us(volatile uint64_t *src)
{
    return __atomic_load_n(src, __ATOMIC_RELAXED);
}

} // namespace

extern "C" int __real_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms);
extern "C" int __real_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms);
extern "C" ssize_t __real_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);

extern "C" int __wrap_esp_transport_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    if (!is_mic_tx_task()) return __real_esp_transport_poll_write(t, timeout_ms);
    const int64_t start = esp_timer_get_time();
    const int ret = __real_esp_transport_poll_write(t, timeout_ms);
    add_us(&s_poll_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" int __wrap_esp_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    if (!is_mic_tx_task()) return __real_esp_transport_write(t, buffer, len, timeout_ms);
    const int64_t start = esp_timer_get_time();
    const int ret = __real_esp_transport_write(t, buffer, len, timeout_ms);
    add_us(&s_transport_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" ssize_t __wrap_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen)
{
    if (!is_mic_tx_task()) return __real_esp_tls_conn_write(tls, data, datalen);
    const int64_t start = esp_timer_get_time();
    const ssize_t ret = __real_esp_tls_conn_write(tls, data, datalen);
    add_us(&s_tls_write_us, (uint64_t)(esp_timer_get_time() - start));
    return ret;
}

extern "C" void websocket_transport_profile_snapshot(uint64_t *poll_us, uint64_t *tls_us, uint64_t *transport_us)
{
    if (poll_us) *poll_us = load_us(&s_poll_write_us);
    if (tls_us) *tls_us = load_us(&s_tls_write_us);
    if (transport_us) *transport_us = load_us(&s_transport_write_us);
}
