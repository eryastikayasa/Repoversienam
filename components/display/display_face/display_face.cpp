#include "display_face.h"

#include <math.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {

static EXT_RAM_BSS_ATTR uint8_t s_face_buffer[DISPLAY_FACE_BUFFER_SIZE] = {0};
static face_state_t s_current_face_state = FACE_IDLE;
static face_state_t s_previous_face_state = FACE_IDLE;
static uint32_t s_state_started_ms = 0;
static uint32_t s_override_until_ms = 0;
static bool s_override_active = false;

// Lightweight deterministic PRNG. No heap, no dynamic state.
static uint32_t s_rng = 0x6D2B79F5u;
static uint32_t s_next_behavior_ms = 0;
static int s_target_gaze_x = 0;
static int s_target_gaze_y = 0;
static int s_target_micro_x = 0;
static int s_target_micro_y = 0;
static int s_current_gaze_x = 0;
static int s_current_gaze_y = 0;
static int s_current_micro_x = 0;
static int s_current_micro_y = 0;

// Internal idle behavior. It is deliberately event/time based: the face can rest.
enum idle_behavior_t : uint8_t {
    IDLE_REST = 0,
    IDLE_GLANCE,
    IDLE_CURIOUS,
    IDLE_MICRO_SHIFT,
};
static idle_behavior_t s_idle_behavior = IDLE_REST;
static uint32_t s_idle_behavior_until_ms = 0;
static bool s_idle_return_pending = false;

// Blink is independent from the main face behavior.
static uint32_t s_next_blink_ms = 0;
static uint32_t s_blink_started_ms = 0;
static uint16_t s_blink_duration_ms = 0;
static uint8_t s_blink_phase = 0; // 0=open, 1=closing, 2=closed, 3=opening
static bool s_blink_double_pending = false;

// Pseudo speech mouth scheduler. No audio samples are inspected.
enum mouth_shape_t : uint8_t {
    MOUTH_CLOSED = 0,
    MOUTH_SMALL,
    MOUTH_MEDIUM,
    MOUTH_WIDE,
};
static mouth_shape_t s_mouth_shape = MOUTH_CLOSED;
static uint32_t s_mouth_shape_until_ms = 0;
static uint8_t s_mouth_shape_count = 0;

// Short behavioral transition between expressions/states.
static uint32_t s_transition_started_ms = 0;
static face_state_t s_transition_from = FACE_IDLE;
static face_state_t s_transition_to = FACE_IDLE;
static constexpr uint32_t FACE_TRANSITION_MS = 220U;

// Only state handoff is protected. Rendering never holds this lock.
static portMUX_TYPE s_face_state_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t rand32(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x ? x : 0xA341316Cu;
    return s_rng;
}

static int rand_range(int min_value, int max_value)
{
    if (max_value <= min_value) return min_value;
    return min_value + (int)(rand32() % (uint32_t)(max_value - min_value + 1));
}

static int clamp_i(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int smooth_step(int current, int target, int amount)
{
    if (current == target) return current;
    const int delta = target - current;
    int step = (delta * amount) / 100;
    if (step == 0) step = delta > 0 ? 1 : -1;
    return current + step;
}

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= DISPLAY_FACE_WIDTH || y < 0 || y >= DISPLAY_FACE_HEIGHT) return;
    uint8_t &byte = s_face_buffer[x + (y >> 3) * DISPLAY_FACE_WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) byte |= mask;
    else byte &= (uint8_t)~mask;
}

static void line(int x0, int y0, int x1, int y1)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fill_circle(int cx, int cy, int radius)
{
    for (int y = -radius; y <= radius; ++y) {
        const int q = radius * radius - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(cx + x, cy + y);
    }
}

static void draw_cat_ears(int offset_y)
{
    line(18, 14 + offset_y, 20, 4 + offset_y);
    line(20, 4 + offset_y, 30, 13 + offset_y);
    line(98, 13 + offset_y, 108, 4 + offset_y);
    line(108, 4 + offset_y, 110, 14 + offset_y);
    line(21, 11 + offset_y, 23, 8 + offset_y);
    line(23, 8 + offset_y, 27, 12 + offset_y);
    line(101, 12 + offset_y, 105, 8 + offset_y);
    line(105, 8 + offset_y, 107, 11 + offset_y);
}

static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y, int openness = 14)
{
    const int eye_radius = clamp_i(openness, 9, 14);
    const int pupil_radius = eye_radius >= 13 ? 7 : 6;
    gaze_x = clamp_i(gaze_x, -7, 7);
    gaze_y = clamp_i(gaze_y, -5, 5);
    const int eye_x = cx + eye_shift_x;
    const int eye_y = cy + eye_shift_y;
    const int pupil_x = eye_x + gaze_x;
    const int pupil_y = eye_y + gaze_y;
    fill_circle(eye_x, eye_y, eye_radius);
    for (int y = -pupil_radius; y <= pupil_radius; ++y) {
        const int q = pupil_radius * pupil_radius - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(pupil_x + x, pupil_y + y, false);
    }
    fill_circle(pupil_x - 2, pupil_y - 3, 2);
}

static void draw_blink_eye(int cx, int cy, int openness)
{
    if (openness <= 0) {
        line(cx - 11, cy, cx + 11, cy);
        return;
    }
    const int half = 5 + openness / 3;
    line(cx - 11, cy + 1, cx - half, cy);
    line(cx - half, cy, cx, cy + 1);
    line(cx, cy + 1, cx + half, cy);
    line(cx + half, cy, cx + 11, cy + 1);
}

static void draw_happy_eye(int cx, int cy)
{
    for (int x = -12; x <= 12; ++x) {
        const float t = (float)x / 12.0f;
        const int y = (int)(6.0f * (1.0f - t * t));
        pixel(cx + x, cy + y);
        if ((x & 1) == 0) pixel(cx + x, cy + y + 1);
    }
}

static void draw_normal_brow(int cx, int cy, int lift = 0)
{
    static const int pts[][2] = {
        {-13, 2}, {-12, 1}, {-11, 0}, {-10, -1}, {-9, -2}, {-8, -3},
        {-7, -4}, {-5, -5}, {-3, -5}, {0, -5}, {3, -5}, {5, -5},
        {7, -4}, {8, -3}, {9, -2}, {10, -1}, {11, 0}, {12, 1}, {13, 2}
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); ++i) {
        pixel(cx + pts[i][0], cy + pts[i][1] - lift);
        pixel(cx + pts[i][0], cy + pts[i][1] + 1 - lift);
    }
}

static void draw_attentive_brow(int cx, int cy)
{
    line(cx - 13, cy + 1, cx - 6, cy - 3);
    line(cx - 6, cy - 3, cx, cy - 4);
    line(cx, cy - 4, cx + 6, cy - 3);
    line(cx + 6, cy - 3, cx + 13, cy + 1);
}

static void draw_thinking_brow(int cx, int cy, bool left)
{
    if (left) line(cx - 13, cy - 1, cx + 10, cy - 5);
    else line(cx - 10, cy - 5, cx + 13, cy - 1);
}

static void draw_sad_brow(int cx, int cy, bool left_eye)
{
    if (left_eye) {
        line(cx - 13, cy - 1, cx, cy + 5);
        line(cx, cy + 5, cx + 13, cy + 9);
    } else {
        line(cx - 13, cy + 9, cx, cy + 5);
        line(cx, cy + 5, cx + 13, cy - 1);
    }
}

static void draw_sad_eye(int cx, int cy, int gaze_y)
{
    const int y = cy + gaze_y;
    line(cx - 11, y, cx - 5, y + 2);
    line(cx - 5, y + 2, cx + 5, y + 2);
    line(cx + 5, y + 2, cx + 11, y);
}

static void draw_sleep_eye(int cx, int cy, int lid)
{
    line(cx - 12, cy, cx + 12, cy);
    if (lid > 0) line(cx - 8, cy + 1, cx + 8, cy + 1);
}

static void draw_error_eye(int cx, int cy, int pulse)
{
    const int size = 9 + pulse;
    for (int i = -size; i <= size; ++i) {
        pixel(cx + i, cy + i);
        pixel(cx + i, cy - i);
    }
}

static void draw_mouth(mouth_shape_t shape, int offset_y)
{
    const int cx = 64;
    const int cy = 53 + offset_y;
    switch (shape) {
        case MOUTH_CLOSED:
            line(cx - 5, cy, cx + 5, cy);
            break;
        case MOUTH_SMALL:
            line(cx - 4, cy - 1, cx, cy + 1);
            line(cx, cy + 1, cx + 4, cy - 1);
            break;
        case MOUTH_MEDIUM:
            line(cx - 5, cy - 2, cx, cy + 3);
            line(cx, cy + 3, cx + 5, cy - 2);
            line(cx - 5, cy - 2, cx + 5, cy - 2);
            break;
        case MOUTH_WIDE:
            line(cx - 7, cy - 2, cx, cy + 4);
            line(cx, cy + 4, cx + 7, cy - 2);
            line(cx - 7, cy - 2, cx + 7, cy - 2);
            line(cx - 3, cy + 1, cx + 3, cy + 1);
            break;
    }
}

static uint32_t elapsed_ms(uint32_t now_ms)
{
    return now_ms - s_state_started_ms;
}

static void choose_idle_behavior(uint32_t now_ms)
{
    const int roll = rand_range(0, 99);
    if (roll < 60) {
        s_idle_behavior = IDLE_REST;
        s_target_gaze_x = 0;
        s_target_gaze_y = 0;
        s_target_micro_x = 0;
        s_target_micro_y = 0;
        s_idle_behavior_until_ms = now_ms + (uint32_t)rand_range(900, 2400);
    } else if (roll < 84) {
        s_idle_behavior = IDLE_GLANCE;
        s_target_gaze_x = rand_range(-5, 5);
        s_target_gaze_y = rand_range(-1, 2);
        s_target_micro_x = 0;
        s_target_micro_y = 0;
        s_idle_behavior_until_ms = now_ms + (uint32_t)rand_range(500, 1200);
    } else if (roll < 92) {
        s_idle_behavior = IDLE_CURIOUS;
        s_target_gaze_x = rand_range(-4, 4);
        s_target_gaze_y = rand_range(-3, -1);
        s_target_micro_x = rand_range(-1, 1);
        s_target_micro_y = 0;
        s_idle_behavior_until_ms = now_ms + (uint32_t)rand_range(400, 900);
    } else {
        s_idle_behavior = IDLE_MICRO_SHIFT;
        s_target_gaze_x = 0;
        s_target_gaze_y = 0;
        s_target_micro_x = rand_range(-1, 1);
        s_target_micro_y = rand_range(-1, 1);
        s_idle_behavior_until_ms = now_ms + (uint32_t)rand_range(350, 750);
    }
    s_idle_return_pending = s_idle_behavior != IDLE_REST;
}

static void schedule_behavior(uint32_t now_ms, face_state_t state)
{
    if (state == FACE_SLEEP) {
        s_target_gaze_x = 0;
        s_target_gaze_y = 0;
        s_target_micro_x = 0;
        s_target_micro_y = 0;
        s_next_behavior_ms = now_ms + 800U;
        return;
    }

    if (state == FACE_IDLE) {
        if (s_next_behavior_ms == 0 || (int32_t)(now_ms - s_next_behavior_ms) >= 0) {
            if (s_idle_behavior == IDLE_REST || (int32_t)(now_ms - s_idle_behavior_until_ms) >= 0) {
                if (s_idle_return_pending) {
                    s_target_gaze_x = 0;
                    s_target_gaze_y = 0;
                    s_target_micro_x = 0;
                    s_target_micro_y = 0;
                    s_idle_return_pending = false;
                    s_idle_behavior = IDLE_REST;
                    s_idle_behavior_until_ms = now_ms + (uint32_t)rand_range(450, 1000);
                } else {
                    choose_idle_behavior(now_ms);
                }
            }
            s_next_behavior_ms = now_ms + 120U;
        }
        return;
    }

    if (state == FACE_LISTENING) {
        if ((int32_t)(now_ms - s_next_behavior_ms) >= 0) {
            s_target_gaze_x = rand_range(-2, 2);
            s_target_gaze_y = rand_range(-1, 1);
            s_target_micro_x = 0;
            s_target_micro_y = 0;
            s_next_behavior_ms = now_ms + (uint32_t)rand_range(900, 1700);
        }
        return;
    }

    if (state == FACE_THINKING) {
        if ((int32_t)(now_ms - s_next_behavior_ms) >= 0) {
            const int choice = rand_range(0, 2);
            if (choice == 0) {
                s_target_gaze_x = 0;
                s_target_gaze_y = -4;
            } else if (choice == 1) {
                s_target_gaze_x = -3;
                s_target_gaze_y = -3;
            } else {
                s_target_gaze_x = 3;
                s_target_gaze_y = -3;
            }
            s_target_micro_x = 0;
            s_target_micro_y = 0;
            s_next_behavior_ms = now_ms + (uint32_t)rand_range(900, 1800);
        }
        return;
    }

    if (state == FACE_SPEAKING) {
        if ((int32_t)(now_ms - s_next_behavior_ms) >= 0) {
            s_target_gaze_x = rand_range(-2, 2);
            s_target_gaze_y = rand_range(-1, 1);
            s_target_micro_x = rand_range(-1, 1);
            s_target_micro_y = 0;
            s_next_behavior_ms = now_ms + (uint32_t)rand_range(700, 1400);
        }
        return;
    }

    s_target_gaze_x = 0;
    s_target_gaze_y = 0;
    s_target_micro_x = 0;
    s_target_micro_y = 0;
    s_next_behavior_ms = now_ms + 1000U;
}

static void update_blink(uint32_t now_ms, face_state_t state)
{
    if (state == FACE_SLEEP) {
        s_blink_phase = 0;
        s_next_blink_ms = now_ms + 1000U;
        return;
    }

    if (s_blink_phase == 0) {
        if (s_next_blink_ms == 0) s_next_blink_ms = now_ms + (uint32_t)rand_range(1800, 5200);
        if ((int32_t)(now_ms - s_next_blink_ms) >= 0) {
            s_blink_phase = 1;
            s_blink_started_ms = now_ms;
            const int style = rand_range(0, 9);
            s_blink_duration_ms = (uint16_t)(style == 0 ? rand_range(90, 125) : rand_range(55, 90));
            s_blink_double_pending = style == 1;
        }
        return;
    }

    const uint32_t t = now_ms - s_blink_started_ms;
    const uint32_t d = s_blink_duration_ms ? s_blink_duration_ms : 70U;
    const uint32_t close_ms = d / 3U;
    const uint32_t open_ms = d / 3U;
    if (t < close_ms) s_blink_phase = 1;
    else if (t < d - open_ms) s_blink_phase = 2;
    else if (t < d) s_blink_phase = 3;
    else {
        s_blink_phase = 0;
        if (s_blink_double_pending) {
            s_blink_double_pending = false;
            s_next_blink_ms = now_ms + 95U;
        } else {
            s_next_blink_ms = now_ms + (uint32_t)rand_range(1800, 5200);
        }
    }
}

static int blink_openness(uint32_t now_ms)
{
    if (s_blink_phase == 0) return 14;
    const uint32_t t = now_ms - s_blink_started_ms;
    const uint32_t d = s_blink_duration_ms ? s_blink_duration_ms : 70U;
    const uint32_t half = d / 2U ? d / 2U : 1U;
    if (t >= d) return 14;
    if (t < half) return (int)(14U - (t * 14U) / half);
    return (int)(((t - half) * 14U) / half);
}

static void update_mouth_scheduler(uint32_t now_ms, face_state_t state)
{
    if (state != FACE_SPEAKING) {
        s_mouth_shape = MOUTH_CLOSED;
        s_mouth_shape_until_ms = now_ms;
        s_mouth_shape_count = 0;
        return;
    }

    if ((int32_t)(now_ms - s_mouth_shape_until_ms) < 0) return;

    mouth_shape_t next = MOUTH_SMALL;
    const int roll = rand_range(0, 99);
    if (s_mouth_shape_count == 0) {
        next = roll < 35 ? MOUTH_CLOSED : MOUTH_SMALL;
    } else if (roll < 28) {
        next = MOUTH_CLOSED;
    } else if (roll < 65) {
        next = MOUTH_SMALL;
    } else if (roll < 90) {
        next = MOUTH_MEDIUM;
    } else {
        next = MOUTH_WIDE;
    }

    // Avoid an obvious repeating ramp while retaining occasional wide speech.
    if (next == s_mouth_shape && next != MOUTH_CLOSED) {
        next = (next == MOUTH_SMALL) ? MOUTH_MEDIUM : MOUTH_SMALL;
    }
    s_mouth_shape = next;
    ++s_mouth_shape_count;
    s_mouth_shape_until_ms = now_ms + (uint32_t)rand_range(60, 160);
}

static void update_motion(uint32_t now_ms, face_state_t state)
{
    schedule_behavior(now_ms, state);
    s_current_gaze_x = smooth_step(s_current_gaze_x, s_target_gaze_x, 18);
    s_current_gaze_y = smooth_step(s_current_gaze_y, s_target_gaze_y, 16);
    s_current_micro_x = smooth_step(s_current_micro_x, s_target_micro_x, 14);
    s_current_micro_y = smooth_step(s_current_micro_y, s_target_micro_y, 14);
}

static void render_face(uint32_t now_ms)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));

    face_state_t state;
    portENTER_CRITICAL(&s_face_state_mux);
    state = s_current_face_state;
    portEXIT_CRITICAL(&s_face_state_mux);

    update_motion(now_ms, state);
    update_blink(now_ms, state);
    update_mouth_scheduler(now_ms, state);

    const uint32_t t = elapsed_ms(now_ms);
    int offset_x = s_current_micro_x;
    int offset_y = s_current_micro_y;
    int gaze_x = s_current_gaze_x;
    int gaze_y = s_current_gaze_y;
    int eye_open = blink_openness(now_ms);
    bool happy_eyes = false;
    bool sad_eyes = false;
    bool sleep_eyes = false;
    bool error_eyes = false;
    bool curious = false;

    switch (state) {
        case FACE_IDLE:
            curious = s_idle_behavior == IDLE_CURIOUS;
            if (s_idle_behavior == IDLE_CURIOUS) offset_x = clamp_i(offset_x, -1, 1);
            break;
        case FACE_LISTENING:
            gaze_x = clamp_i(gaze_x, -3, 3);
            gaze_y = clamp_i(gaze_y, -2, 2);
            offset_y -= 1;
            eye_open = s_blink_phase == 0 ? 14 : eye_open;
            break;
        case FACE_THINKING:
            gaze_y = clamp_i(gaze_y, -5, -2);
            break;
        case FACE_SPEAKING:
            gaze_x = clamp_i(gaze_x, -3, 3);
            gaze_y = clamp_i(gaze_y, -2, 2);
            break;
        case FACE_HAPPY:
            happy_eyes = true;
            if (t < 220U) offset_y -= (int)((220U - t) / 110U);
            break;
        case FACE_SAD:
            sad_eyes = true;
            gaze_y = 3;
            offset_y += 1;
            break;
        case FACE_ERROR:
            error_eyes = true;
            if (t < 500U) offset_x += ((t / 140U) & 1U) ? 1 : -1;
            break;
        case FACE_SLEEP:
            sleep_eyes = true;
            gaze_x = 0;
            gaze_y = 0;
            offset_x = 0;
            offset_y += ((t / 1800U) & 1U) ? 0 : 1;
            break;
        default:
            break;
    }

    if ((int32_t)(now_ms - s_transition_started_ms) < (int32_t)FACE_TRANSITION_MS) {
        const uint32_t dt = now_ms - s_transition_started_ms;
        const int pulse = dt < FACE_TRANSITION_MS / 2U ? 1 : 0;
        if (s_transition_to == FACE_LISTENING) offset_y -= pulse;
        else if (s_transition_to == FACE_SPEAKING) offset_y += pulse;
        else if (s_transition_to == FACE_ERROR) offset_x += pulse;
    }

    draw_cat_ears(offset_y);
    const int left_x = 34 + offset_x;
    const int right_x = 94 + offset_x;
    const int eye_y = 28 + offset_y;

    if (sleep_eyes) {
        draw_sleep_eye(left_x, eye_y + 1, 1);
        draw_sleep_eye(right_x, eye_y + 1, 1);
    } else if (error_eyes) {
        const int pulse = (t < 600U && ((t / 260U) & 1U)) ? 1 : 0;
        draw_error_eye(left_x, eye_y, pulse);
        draw_error_eye(right_x, eye_y, pulse);
    } else if (happy_eyes) {
        draw_happy_eye(left_x, eye_y);
        draw_happy_eye(right_x, eye_y);
    } else if (sad_eyes) {
        draw_sad_eye(left_x, eye_y, gaze_y);
        draw_sad_eye(right_x, eye_y, gaze_y);
    } else if (s_blink_phase != 0) {
        draw_blink_eye(left_x, eye_y, eye_open);
        draw_blink_eye(right_x, eye_y, eye_open);
    } else {
        const int openness = state == FACE_THINKING ? 13 : 14;
        draw_open_eye(left_x, eye_y, gaze_x, gaze_y, 0, 0, openness);
        draw_open_eye(right_x, eye_y, gaze_x, gaze_y, 0, 0, openness);
    }

    const int brow_y = 14 + offset_y;
    switch (state) {
        case FACE_LISTENING:
            draw_attentive_brow(left_x, brow_y);
            draw_attentive_brow(right_x, brow_y);
            break;
        case FACE_THINKING:
            draw_thinking_brow(left_x, brow_y, true);
            draw_thinking_brow(right_x, brow_y, false);
            break;
        case FACE_SAD:
            draw_sad_brow(left_x, brow_y, true);
            draw_sad_brow(right_x, brow_y, false);
            break;
        case FACE_ERROR:
            draw_normal_brow(left_x, brow_y, 3);
            draw_normal_brow(right_x, brow_y, 3);
            break;
        case FACE_IDLE:
            draw_normal_brow(left_x, brow_y, curious ? 2 : 0);
            draw_normal_brow(right_x, brow_y, curious ? 2 : 0);
            break;
        default:
            draw_normal_brow(left_x, brow_y, state == FACE_HAPPY ? 1 : 0);
            draw_normal_brow(right_x, brow_y, state == FACE_HAPPY ? 1 : 0);
            break;
    }

    if (state == FACE_HAPPY) {
        draw_mouth(MOUTH_MEDIUM, offset_y);
        // Smile is intentionally stable after the entry bounce.
        for (int x = -10; x <= 10; ++x) {
            const float q = (float)x / 10.0f;
            const int y = (int)(5.0f * (1.0f - q * q));
            pixel(64 + x, 53 + offset_y + y);
        }
    } else if (state == FACE_SAD) {
        line(55, 56 + offset_y, 64, 53 + offset_y);
        line(64, 53 + offset_y, 73, 56 + offset_y);
    } else if (state == FACE_ERROR) {
        line(57, 55 + offset_y, 62, 52 + offset_y);
        line(62, 52 + offset_y, 67, 55 + offset_y);
        line(67, 55 + offset_y, 72, 52 + offset_y);
    } else if (state == FACE_SLEEP) {
        draw_mouth(MOUTH_CLOSED, offset_y);
    } else {
        draw_mouth(state == FACE_SPEAKING ? s_mouth_shape : MOUTH_CLOSED, offset_y);
    }
}

} // namespace

void display_face_init(void)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_face_state_mux);
    s_current_face_state = FACE_IDLE;
    s_previous_face_state = FACE_IDLE;
    s_state_started_ms = now_ms;
    s_override_until_ms = 0;
    s_override_active = false;
    portEXIT_CRITICAL(&s_face_state_mux);

    s_rng ^= now_ms + 0x9E3779B9u;
    s_next_behavior_ms = now_ms + 500U;
    s_idle_behavior = IDLE_REST;
    s_idle_behavior_until_ms = now_ms + 1200U;
    s_idle_return_pending = false;
    s_next_blink_ms = now_ms + (uint32_t)rand_range(1800, 4200);
    s_blink_phase = 0;
    s_current_gaze_x = 0;
    s_current_gaze_y = 0;
    s_target_gaze_x = 0;
    s_target_gaze_y = 0;
    s_current_micro_x = 0;
    s_current_micro_y = 0;
    s_target_micro_x = 0;
    s_target_micro_y = 0;
    s_mouth_shape = MOUTH_CLOSED;
    s_mouth_shape_until_ms = now_ms;
    s_mouth_shape_count = 0;
    s_transition_started_ms = now_ms;
    s_transition_from = FACE_IDLE;
    s_transition_to = FACE_IDLE;
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
    render_face(now_ms);
}

void display_face_update(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_face_state_mux);
    if (s_override_active && (int32_t)(now_ms - s_override_until_ms) >= 0) {
        s_override_active = false;
        s_current_face_state = s_previous_face_state;
        s_state_started_ms = now_ms;
        s_next_behavior_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_face_state_mux);
    render_face(now_ms);
}

void display_face_set_state(face_state_t state)
{
    if (state < FACE_IDLE || state > FACE_SLEEP) state = FACE_IDLE;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    portENTER_CRITICAL(&s_face_state_mux);
    if (state == s_current_face_state) {
        portEXIT_CRITICAL(&s_face_state_mux);
        return;
    }
    s_transition_from = s_current_face_state;
    s_transition_to = state;
    s_transition_started_ms = now_ms;
    s_previous_face_state = s_current_face_state;
    s_current_face_state = state;
    s_state_started_ms = now_ms;
    s_override_active = false;
    portEXIT_CRITICAL(&s_face_state_mux);

    // Animation state is display-task owned; reset only the next behavior deadline.
    s_next_behavior_ms = now_ms + 350U;
    s_target_micro_x = 0;
    s_target_micro_y = 0;
    if (state == FACE_SPEAKING) s_mouth_shape_until_ms = now_ms;
}

void display_face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    if (state < FACE_IDLE || state > FACE_SLEEP) state = FACE_IDLE;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (duration_ms == 0) {
        display_face_set_state(state);
        return;
    }

    portENTER_CRITICAL(&s_face_state_mux);
    s_previous_face_state = s_current_face_state;
    s_transition_from = s_current_face_state;
    s_transition_to = state;
    s_transition_started_ms = now_ms;
    s_current_face_state = state;
    s_state_started_ms = now_ms;
    s_override_until_ms = now_ms + duration_ms;
    s_override_active = true;
    portEXIT_CRITICAL(&s_face_state_mux);

    s_next_behavior_ms = now_ms + 350U;
    s_target_micro_x = 0;
    s_target_micro_y = 0;
    if (state == FACE_SPEAKING) s_mouth_shape_until_ms = now_ms;
}

face_state_t display_face_get_state(void)
{
    face_state_t state;
    portENTER_CRITICAL(&s_face_state_mux);
    state = s_current_face_state;
    portEXIT_CRITICAL(&s_face_state_mux);
    return state;
}

void display_face_render_mochi_gaze(int expr, int step,
                                    int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
    draw_cat_ears(sY);
    const int left_x = 34 + sX + eye_shift_x;
    const int right_x = 94 + sX + eye_shift_x;
    const int eye_y = 28 + sY + eye_shift_y;
    if (expr == 6) {
        draw_sad_brow(left_x, 14 + sY, true);
        draw_sad_brow(right_x, 14 + sY, false);
        draw_sad_eye(left_x, eye_y, gaze_y);
        draw_sad_eye(right_x, eye_y, gaze_y);
        draw_mouth(MOUTH_CLOSED, sY);
    } else if (expr == 2 && step == 2) {
        draw_happy_eye(left_x, eye_y);
        draw_happy_eye(right_x, eye_y);
        draw_normal_brow(left_x, 14 + sY, 1);
        draw_normal_brow(right_x, 14 + sY, 1);
        draw_mouth(MOUTH_MEDIUM, sY);
    } else if (expr == 99) {
        draw_error_eye(left_x, eye_y, 0);
        draw_error_eye(right_x, eye_y, 0);
        draw_normal_brow(left_x, 14 + sY, 3);
        draw_normal_brow(right_x, 14 + sY, 3);
        draw_mouth(MOUTH_MEDIUM, sY);
    } else if (step == 3) {
        draw_sleep_eye(left_x, eye_y, 1);
        draw_sleep_eye(right_x, eye_y, 1);
        draw_mouth(MOUTH_CLOSED, sY);
    } else if (step == 1) {
        draw_blink_eye(left_x, eye_y, 0);
        draw_blink_eye(right_x, eye_y, 0);
        draw_normal_brow(left_x, 14 + sY);
        draw_normal_brow(right_x, 14 + sY);
        draw_mouth(MOUTH_CLOSED, sY);
    } else {
        draw_open_eye(left_x, eye_y, gaze_x, gaze_y, 0, 0, expr == 1 ? 14 : 13);
        draw_open_eye(right_x, eye_y, gaze_x, gaze_y, 0, 0, expr == 1 ? 14 : 13);
        if (expr == 1) {
            draw_attentive_brow(left_x, 14 + sY);
            draw_attentive_brow(right_x, 14 + sY);
            draw_mouth(MOUTH_SMALL, sY);
        } else {
            draw_normal_brow(left_x, 14 + sY);
            draw_normal_brow(right_x, 14 + sY);
            draw_mouth(MOUTH_CLOSED, sY);
        }
    }
}

void display_face_render_mochi(int expr, int step,
                               int sX, int sY, int arahLirik)
{
    const int gaze = clamp_i(arahLirik, -7, 7);
    display_face_render_mochi_gaze(expr, step, sX, sY, gaze, 0, 0, 0);
}

void display_face_render(void)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    render_face(now_ms);
}

const uint8_t *display_face_buffer(void)
{
    return s_face_buffer;
}
