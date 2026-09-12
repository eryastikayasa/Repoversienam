#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wakeword_detected_cb_t)(void *ctx);

bool wakeword_init(void);
bool wakeword_start(wakeword_detected_cb_t cb, void *ctx);
bool wakeword_is_ready(void);

#ifdef __cplusplus
}
#endif
