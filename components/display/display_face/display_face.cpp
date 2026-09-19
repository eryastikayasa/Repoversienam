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

struct spring1d_t {
    float current = 0.0f;
    float target = 0.0f;
    float velocity = 0.0f;

    void reset(float value = 0.0f)
    {
        current = value;
        target = value;
        velocity = 0.0f;
    }

    void update(float dt, float omega, float zeta)
    {
        if (dt <= 0.0f) return;
        if (dt > 0.033f) dt = 0.033f;

        const float f = 1.0f + 2.0f * dt * zeta * omega;
        const float oo = omega * omega;
        const float h = dt * oo;
        const float det = f + dt * h;

        const float new_velocity =
            (velocity + h * (target - current)) / det;

        current += new_velocity * dt;
        velocity = new_velocity;
    }
};

// Second-order motion keeps the eye shape and pupil alive without heap use.
// The eye moves first; the pupil follows with a small physical lag.
static spring1d_t s_eye_motion_x;
static spring1d_t s_eye_motion_y;
static spring1d_t s_pupil_motion_x;
static spring1d_t s_pupil_motion_y;

// Eyebrow springs: expressive, subtle, and coupled to the same face motion.
static spring1d_t s_brow_left_y;
static spring1d_t s_brow_right_y;
static spring1d_t s_brow_left_angle;
static spring1d_t s_brow_right_angle;
static uint32_t s_last_motion_ms = 0;

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

// Continuous speech aperture: the scheduler selects syllable-like targets,
// while a spring makes the mouth visibly open/close instead of snapping.
static spring1d_t s_mouth_motion;

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

static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y, int openness = 14,
                          float tilt = 0.0f)
{
    const int eye_radius = clamp_i(openness, 9, 14);
    const int pupil_radius = eye_radius >= 13 ? 5 : 4;
    gaze_x = clamp_i(gaze_x, -7, 7);
    gaze_y = clamp_i(gaze_y, -5, 5);

    const int eye_x = cx + eye_shift_x;
    const int eye_y = cy + eye_shift_y;

    // Organic 1-bit OLED eye silhouette.
    const float ct = cosf(tilt);
    const float st = sinf(tilt);
    for (int y = -eye_radius; y <= eye_radius; ++y) {
        for (int x = -eye_radius; x <= eye_radius; ++x) {
            const float ex = (float)x / (float)eye_radius;
            const float ey = (float)y / (float)eye_radius;
            const float shape = ex * ex + ey * ey;
            if (shape <= 1.0f) {
                const float rx = (float)x * ct - (float)y * st;
                const float ry = (float)x * st + (float)y * ct;
                const float organic = 1.0f
                    + 0.045f * sinf((float)(x + y) * 0.55f)
                    + 0.035f * cosf((float)y * 0.70f);
                if ((rx * rx + ry * ry) <=
                    (float)(eye_radius * eye_radius) * organic) {
                    pixel(eye_x + x, eye_y + y);
                }
            }
        }
    }

    // Pupil has its own smooth tracking, while following the moving eyeball.
    const int pupil_x = eye_x + gaze_x;
    const int pupil_y = eye_y + gaze_y;
    fill_circle(pupil_x, pupil_y, pupil_radius);
    for (int y = -pupil_radius; y <= pupil_radius; ++y) {
        const int q = pupil_radius * pupil_radius - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(pupil_x + x, pupil_y + y, false);
    }
    // Small OLED catchlight.
    pixel(pupil_x - 1, pupil_y - 2);
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

static void draw_happy_eye(int cx, int cy,
                            int gaze_x = 0, int gaze_y = 0,
                            int eye_shift_x = 0, int eye_shift_y = 0,
                            float tilt = 0.0f)
{
    draw_open_eye(cx, cy, gaze_x, gaze_y,
                  eye_shift_x, eye_shift_y, 7, tilt);
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

static void draw_sad_eye(int cx, int cy, int gaze_y,
                          int eye_shift_x = 0, int eye_shift_y = 0,
                          float tilt = 0.0f)
{
    draw_open_eye(cx, cy, 0, gaze_y,
                  eye_shift_x, eye_shift_y, 10, tilt);
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
    const int cy = 51 + offset_y;

    // Mouth language follows the supplied OLED reference:
    // compact, thick 1-bit curves rather than a large cartoon mouth.
    switch (shape) {
        case MOUTH_CLOSED:
            line(cx - 7, cy + 1, cx - 3, cy);
            line(cx - 3, cy, cx + 3, cy);
            line(cx + 3, cy, cx + 7, cy + 1);
            line(cx - 3, cy + 3, cx + 3, cy + 3);
            break;

        case MOUTH_SMALL:
            line(cx - 6, cy + 1, cx - 2, cy);
            line(cx - 2, cy, cx + 2, cy);
            line(cx + 2, cy, cx + 6, cy + 1);
            break;

        case MOUTH_MEDIUM:
            line(cx - 7, cy, cx - 3, cy - 2);
            line(cx - 3, cy - 2, cx + 3, cy - 2);
            line(cx + 3, cy - 2, cx + 7, cy);
            line(cx - 4, cy + 3, cx + 4, cy + 3);
            break;

        case MOUTH_WIDE:
            line(cx - 8, cy - 1, cx - 4, cy - 3);
            line(cx - 4, cy - 3, cx + 4, cy - 3);
            line(cx + 4, cy - 3, cx + 8, cy - 1);
            line(cx - 6, cy + 2, cx + 6, cy + 2);
            break;
    }
}

static void draw_speaking_mouth(float aperture, int offset_y)
{
    const int cx = 64;
    const int cy = 51 + offset_y;

    aperture = aperture < 0.0f ? 0.0f : (aperture > 1.0f ? 1.0f : aperture);

    // Compact 1-bit mouth. The opening breathes between nearly closed and
    // wide speech shapes; no large cartoon mouth is introduced.
    const int half_w = 4 + (int)lroundf(aperture * 4.0f);
    const int top = cy - 1 - (int)lroundf(aperture * 3.0f);
    const int bottom = cy + 1 + (int)lroundf(aperture * 3.0f);

    line(cx - half_w, top, cx - half_w / 2, top - 1);
    line(cx - half_w / 2, top - 1, cx + half_w / 2, top - 1);
    line(cx + half_w / 2, top - 1, cx + half_w, top);

    if (aperture > 0.18f) {
        line(cx - half_w, bottom, cx + half_w, bottom);
    }

    // A tiny center contraction keeps fast speech from looking like a static
    // open rectangle at low OLED resolution.
    if (aperture > 0.65f) {
        line(cx - 3, cy + 1, cx + 3, cy + 1);
    }
}

static void draw_brow_physics(float cx, float cy, float length,
                              float angle_deg, float offset_y)
{
    const float rad = angle_deg * (3.14159265358979323846f / 180.0f);
    const float half = length * 0.5f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);

    const int x1 = (int)lroundf(cx - half * cs);
    const int y1 = (int)lroundf(cy + offset_y - half * sn);
    const int x2 = (int)lroundf(cx + half * cs);
    const int y2 = (int)lroundf(cy + offset_y + half * sn);

    // Two-pixel OLED stroke gives the thick eyebrow seen in the reference.
    line(x1, y1, x2, y2);
    line(x1, y1 + 1, x2, y2 + 1);
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

static float mouth_shape_aperture(mouth_shape_t shape)
{
    switch (shape) {
        case MOUTH_CLOSED: return 0.04f;
        case MOUTH_SMALL:  return 0.28f;
        case MOUTH_MEDIUM: return 0.62f;
        case MOUTH_WIDE:   return 1.0f;
        default:           return 0.04f;
    }
}

static void update_mouth_scheduler(uint32_t now_ms, face_state_t state)
{
    if (state != FACE_SPEAKING) {
        s_mouth_shape = MOUTH_CLOSED;
        s_mouth_shape_until_ms = now_ms;
        s_mouth_shape_count = 0;
        s_mouth_motion.target = 0.0f;
        s_mouth_motion.update(0.016f, 20.0f, 0.90f);
        return;
    }

    if ((int32_t)(now_ms - s_mouth_shape_until_ms) >= 0) {
        mouth_shape_t next;
        const int roll = rand_range(0, 99);

        // Short syllable-like rhythm: mostly small/medium openings, with
        // occasional closure and wide peaks. Avoid the same shape twice.
        if (s_mouth_shape_count == 0) {
            next = roll < 22 ? MOUTH_CLOSED : MOUTH_SMALL;
        } else if (roll < 12) {
            next = MOUTH_CLOSED;
        } else if (roll < 54) {
            next = MOUTH_SMALL;
        } else if (roll < 88) {
            next = MOUTH_MEDIUM;
        } else {
            next = MOUTH_WIDE;
        }

        if (next == s_mouth_shape) {
            if (next == MOUTH_SMALL) next = MOUTH_MEDIUM;
            else if (next == MOUTH_MEDIUM) next = MOUTH_SMALL;
            else if (next == MOUTH_WIDE) next = MOUTH_MEDIUM;
            else next = MOUTH_SMALL;
        }

        s_mouth_shape = next;
        ++s_mouth_shape_count;
        s_mouth_motion.target = mouth_shape_aperture(next);

        // 55-125 ms produces visible speech movement at the Repo7 ~20 FPS
        // display cadence without turning the mouth into noisy jitter.
        s_mouth_shape_until_ms = now_ms + (uint32_t)rand_range(55, 125);
    }

    float dt = 0.016f;
    static uint32_t last_ms = 0;
    if (last_ms != 0) {
        dt = (float)(now_ms - last_ms) * 0.001f;
        if (dt < 0.008f) dt = 0.008f;
        if (dt > 0.033f) dt = 0.033f;
    }
    last_ms = now_ms;
    s_mouth_motion.update(dt, 24.0f, 0.82f);
}

static void update_brow_targets(face_state_t state)
{
    // Coordinates are relative to the eye center. The eyebrow is deliberately
    // compact and expressive, matching the supplied 128x64 reference.
    switch (state) {
        case FACE_IDLE:
            s_brow_left_y.target = -16.0f;
            s_brow_right_y.target = -16.0f;
            s_brow_left_angle.target = -3.0f;
            s_brow_right_angle.target = 3.0f;
            break;

        case FACE_LISTENING:
            s_brow_left_y.target = -18.0f;
            s_brow_right_y.target = -18.0f;
            s_brow_left_angle.target = -2.0f;
            s_brow_right_angle.target = 2.0f;
            break;

        case FACE_THINKING:
            // Asymmetric Vector-like thinking pose.
            s_brow_left_y.target = -15.0f;
            s_brow_right_y.target = -19.0f;
            s_brow_left_angle.target = 18.0f;
            s_brow_right_angle.target = -10.0f;
            break;

        case FACE_SPEAKING:
            s_brow_left_y.target = -17.0f;
            s_brow_right_y.target = -17.0f;
            s_brow_left_angle.target = 0.0f;
            s_brow_right_angle.target = 0.0f;
            break;

        case FACE_HAPPY:
            s_brow_left_y.target = -19.0f;
            s_brow_right_y.target = -19.0f;
            s_brow_left_angle.target = -8.0f;
            s_brow_right_angle.target = 8.0f;
            break;

        case FACE_SAD:
            s_brow_left_y.target = -14.0f;
            s_brow_right_y.target = -14.0f;
            s_brow_left_angle.target = -18.0f;
            s_brow_right_angle.target = 18.0f;
            break;

        case FACE_ERROR:
            s_brow_left_y.target = -15.0f;
            s_brow_right_y.target = -15.0f;
            s_brow_left_angle.target = 20.0f;
            s_brow_right_angle.target = -20.0f;
            break;

        case FACE_SLEEP:
            s_brow_left_y.target = -16.0f;
            s_brow_right_y.target = -16.0f;
            s_brow_left_angle.target = 0.0f;
            s_brow_right_angle.target = 0.0f;
            break;
    }
}

static void update_motion(uint32_t now_ms, face_state_t state)
{
    schedule_behavior(now_ms, state);
    update_brow_targets(state);

    float dt = 0.016f;
    if (s_last_motion_ms != 0) {
        dt = (float)(now_ms - s_last_motion_ms) * 0.001f;
        if (dt < 0.008f) dt = 0.008f;
        if (dt > 0.033f) dt = 0.033f;
    }
    s_last_motion_ms = now_ms;

    // The white eye shape has a smaller, slower excursion.
    s_eye_motion_x.target =
        (float)s_target_gaze_x * 0.45f + (float)s_target_micro_x * 0.80f;
    s_eye_motion_y.target =
        (float)s_target_gaze_y * 0.38f + (float)s_target_micro_y * 0.80f;

    s_eye_motion_x.target = (float)clamp_i((int)lroundf(s_eye_motion_x.target), -3, 3);
    s_eye_motion_y.target = (float)clamp_i((int)lroundf(s_eye_motion_y.target), -2, 2);

    // Pupil follows the intended gaze and slightly follows eye inertia.
    s_pupil_motion_x.target =
        (float)s_target_gaze_x + s_eye_motion_x.current * 0.18f;
    s_pupil_motion_y.target =
        (float)s_target_gaze_y + s_eye_motion_y.current * 0.18f;

    s_eye_motion_x.update(dt, 10.5f, 0.72f);
    s_eye_motion_y.update(dt, 10.5f, 0.72f);
    s_pupil_motion_x.update(dt, 18.0f, 0.82f);
    s_pupil_motion_y.update(dt, 18.0f, 0.82f);

    s_brow_left_y.update(dt, 16.0f, 0.80f);
    s_brow_right_y.update(dt, 16.0f, 0.80f);
    s_brow_left_angle.update(dt, 16.0f, 0.80f);
    s_brow_right_angle.update(dt, 16.0f, 0.80f);

    s_current_gaze_x = clamp_i((int)lroundf(s_pupil_motion_x.current), -6, 6);
    s_current_gaze_y = clamp_i((int)lroundf(s_pupil_motion_y.current), -5, 5);

    // Keep the legacy integer micro state for the transition/legacy renderer.
    s_current_micro_x = smooth_step(s_current_micro_x, s_target_micro_x, 18);
    s_current_micro_y = smooth_step(s_current_micro_y, s_target_micro_y, 18);
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

    switch (state) {
        case FACE_IDLE:
            if (s_idle_behavior == IDLE_CURIOUS) offset_x = clamp_i(offset_x, -1, 1);
            break;
        case FACE_LISTENING:
            gaze_x = clamp_i(gaze_x, -3, 3);
            gaze_y = clamp_i(gaze_y, -2, 2);
            // Keep the eye baseline fixed; listening is expressed by gaze/blink,
            // not by moving the whole face vertically.
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

    const int eye_motion_x = clamp_i((int)lroundf(s_eye_motion_x.current), -3, 3);
    const int eye_motion_y = clamp_i((int)lroundf(s_eye_motion_y.current), -2, 2);
    const float eye_tilt = clamp_i((int)lroundf(s_eye_motion_x.velocity * 0.35f), -2, 2) * 0.035f;

    const int left_x = 34 + offset_x;
    const int right_x = 94 + offset_x;
    const int eye_y = 28 + offset_y;

    if (sleep_eyes) {
        draw_sleep_eye(left_x, eye_y, 1);
        draw_sleep_eye(right_x, eye_y, 1);
    } else if (error_eyes) {
        const int pulse = (t < 600U && ((t / 260U) & 1U)) ? 1 : 0;
        draw_error_eye(left_x, eye_y, pulse);
        draw_error_eye(right_x, eye_y, pulse);
    } else if (happy_eyes) {
        draw_happy_eye(left_x, eye_y, s_current_gaze_x, s_current_gaze_y,
                       eye_motion_x, eye_motion_y, eye_tilt);
        draw_happy_eye(right_x, eye_y, s_current_gaze_x, s_current_gaze_y,
                       eye_motion_x, eye_motion_y, eye_tilt);
    } else if (sad_eyes) {
        draw_sad_eye(left_x, eye_y, s_current_gaze_y,
                     eye_motion_x, eye_motion_y, eye_tilt);
        draw_sad_eye(right_x, eye_y, s_current_gaze_y,
                     eye_motion_x, eye_motion_y, eye_tilt);
    } else if (s_blink_phase != 0) {
        draw_blink_eye(left_x, eye_y, eye_open);
        draw_blink_eye(right_x, eye_y, eye_open);
    } else {
        const int openness = state == FACE_THINKING ? 13 : 14;
        draw_open_eye(left_x, eye_y,
                      clamp_i(gaze_x, -5, 5), clamp_i(gaze_y, -4, 4),
                      eye_motion_x, eye_motion_y, openness, eye_tilt);
        draw_open_eye(right_x, eye_y,
                      clamp_i(gaze_x, -5, 5), clamp_i(gaze_y, -4, 4),
                      eye_motion_x, eye_motion_y, openness, eye_tilt);
    }

    // Eyebrows are now part of the active renderer. They use springs so
    // expression changes ease into place instead of snapping.
    if (state != FACE_SLEEP) {
        const float brow_y_base = (float)eye_y;
        const float brow_left_y = s_brow_left_y.current + offset_y * 0.15f;
        const float brow_right_y = s_brow_right_y.current + offset_y * 0.15f;

        draw_brow_physics((float)left_x, brow_y_base, 24.0f,
                          s_brow_left_angle.current, brow_left_y);
        draw_brow_physics((float)right_x, brow_y_base, 24.0f,
                          s_brow_right_angle.current, brow_right_y);
    }

    if (state == FACE_HAPPY) {
        draw_mouth(MOUTH_MEDIUM, offset_y);
        // Smile is intentionally stable after the entry bounce.
        for (int x = -10; x <= 10; ++x) {
            const float q = (float)x / 10.0f;
            const int y = (int)(5.0f * (1.0f - q * q));
            pixel(64 + x, 51 + offset_y + y);
        }
    } else if (state == FACE_SAD) {
        line(55, 54 + offset_y, 64, 51 + offset_y);
        line(64, 51 + offset_y, 73, 54 + offset_y);
    } else if (state == FACE_ERROR) {
        line(57, 53 + offset_y, 62, 50 + offset_y);
        line(62, 50 + offset_y, 67, 53 + offset_y);
        line(67, 53 + offset_y, 72, 50 + offset_y);
    } else if (state == FACE_SLEEP) {
        draw_mouth(MOUTH_CLOSED, offset_y);
    } else if (state == FACE_SPEAKING) {
        draw_speaking_mouth(s_mouth_motion.current, offset_y);
    } else {
        draw_mouth(MOUTH_CLOSED, offset_y);
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
    s_eye_motion_x.reset(0.0f);
    s_eye_motion_y.reset(0.0f);
    s_pupil_motion_x.reset(0.0f);
    s_pupil_motion_y.reset(0.0f);
    s_brow_left_y.reset(-16.0f);
    s_brow_right_y.reset(-16.0f);
    s_brow_left_angle.reset(0.0f);
    s_brow_right_angle.reset(0.0f);
    s_last_motion_ms = now_ms;
    s_mouth_shape = MOUTH_CLOSED;
    s_mouth_shape_until_ms = now_ms;
    s_mouth_shape_count = 0;
    s_mouth_motion.reset(0.04f);
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
    s_eye_motion_x.target = 0.0f;
    s_eye_motion_y.target = 0.0f;
    if (state == FACE_SPEAKING) {
        s_mouth_shape_until_ms = now_ms;
        s_mouth_shape_count = 0;
    }
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

const uint8_t *display_face_buffer(void)
{
    return s_face_buffer;
}
