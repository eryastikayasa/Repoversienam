#pragma once

typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

void gemini_tool_handle_call(const cJSON *tool_call);

#ifdef __cplusplus
}
#endif
