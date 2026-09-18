#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wakeword_init(void);
bool wakeword_start(void);
void wakeword_stop(void);
bool wakeword_is_running(void);
bool wakeword_detected(void);
void wakeword_clear_detected(void);
int wakeword_get_chunk_samples(void);
int wakeword_get_sample_rate(void);
void wakeword_deinit(void);

#ifdef __cplusplus
}
#endif
