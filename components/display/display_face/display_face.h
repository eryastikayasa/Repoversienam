#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACE_IDLE = 0,
    FACE_LISTENING,
    FACE_THINKING,
    FACE_SPEAKING,
    FACE_HAPPY,
    FACE_SAD,
    FACE_ERROR,
    FACE_SLEEP
} face_state_t;

#define DISPLAY_FACE_WIDTH   128
#define DISPLAY_FACE_HEIGHT  64
#define DISPLAY_FACE_BUFFER_SIZE (DISPLAY_FACE_WIDTH * DISPLAY_FACE_HEIGHT / 8)

// Face is a framebuffer producer only.
// It does not know about I2C, SSD1306, or display_driver.
void display_face_init(void);
void display_face_update(uint32_t now_ms);
void display_face_set_state(face_state_t state);
void display_face_show_for_ms(face_state_t state, uint32_t duration_ms);
face_state_t display_face_get_state(void);

void display_face_render_mochi_gaze(int expr, int step,
                                    int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y);
void display_face_render_mochi(int expr, int step,
                               int sX, int sY, int arahLirik);
void display_face_render(void);

// Read-only framebuffer for the future Display Engine.
const uint8_t *display_face_buffer(void);

#ifdef __cplusplus
}
#endif
