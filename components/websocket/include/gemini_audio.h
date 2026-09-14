#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

bool gemini_audio_process_server_root(const cJSON *root, uint32_t generation);

/* Compatibility entry point for non-RX callers. */
bool gemini_audio_process_server_message(const char *json, size_t len, uint32_t generation);

#ifdef __cplusplus
}
#endif
