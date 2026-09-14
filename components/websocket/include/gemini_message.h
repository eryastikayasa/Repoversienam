#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GEMINI_MESSAGE_UNKNOWN = 0,
    GEMINI_MESSAGE_SETUP,
    GEMINI_MESSAGE_SERVER_CONTENT,
    GEMINI_MESSAGE_SESSION_RESUMPTION,
    GEMINI_MESSAGE_GOAWAY,
    GEMINI_MESSAGE_ERROR
} gemini_message_type_t;

gemini_message_type_t gemini_message_classify(const char *json, size_t len);
gemini_message_type_t gemini_message_classify_root(const cJSON *root);

#ifdef __cplusplus
}
#endif
