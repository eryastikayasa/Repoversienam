#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gemini_protocol_on_connected(void);
void gemini_protocol_on_disconnected(void);
bool gemini_protocol_process_message(const char *json, size_t len, uint32_t generation);
bool gemini_protocol_setup_complete(void);
bool gemini_protocol_greeting_finished(void);
bool gemini_protocol_should_resume(void);
bool gemini_protocol_take_resume_request(void);
uint64_t gemini_protocol_goaway_time_left_ms(void);

/* Mark the current session as intentionally ended. This clears any pending
 * session-resumption request so app_startup will not reconnect automatically. */
void gemini_protocol_request_intentional_end(void);

#ifdef __cplusplus
}
#endif
