#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_text_init(void);
void display_text_update(uint32_t now_ms);
void display_text_set_user(const char *text);
void display_text_set_gemini(const char *text);
void display_text_append_user(const char *text);
void display_text_append_gemini(const char *text);
void display_text_set_status(const char *text);
void display_text_render_user(void);
void display_text_render_gemini(void);
void display_text_render_status(void);
const uint8_t *display_text_buffer(void);

#ifdef __cplusplus
}
#endif
