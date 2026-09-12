#include "display_face.h"

#include <math.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"

namespace {

static EXT_RAM_BSS_ATTR uint8_t s_face_buffer[DISPLAY_FACE_BUFFER_SIZE] = {0};
static face_state_t s_current_face_state = FACE_IDLE;
static face_state_t s_previous_face_state = FACE_IDLE;
static uint32_t s_state_started_ms = 0;
static uint32_t s_override_until_ms = 0;
static bool s_override_active = false;

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= DISPLAY_FACE_WIDTH || y < 0 || y >= DISPLAY_FACE_HEIGHT) return;
    uint8_t &byte = s_face_buffer[x + (y >> 3) * DISPLAY_FACE_WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) byte |= mask;
    else byte &= (uint8_t)~mask;
}

static void fill_circle(int cx, int cy, int radius)
{
    for (int y = -radius; y <= radius; ++y) {
        const int q = radius * radius - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(cx + x, cy + y);
    }
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

static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y)
{
    constexpr int EYE_RADIUS = 14;
    constexpr int PUPIL_RADIUS = 7;
    if (gaze_x < -7) gaze_x = -7;
    if (gaze_x > 7) gaze_x = 7;
    if (gaze_y < -5) gaze_y = -5;
    if (gaze_y > 5) gaze_y = 5;
    const int eye_x = cx + eye_shift_x;
    const int eye_y = cy + eye_shift_y;
    const int pupil_x = eye_x + gaze_x;
    const int pupil_y = eye_y + gaze_y;
    fill_circle(eye_x, eye_y, EYE_RADIUS);
    for (int y = -PUPIL_RADIUS; y <= PUPIL_RADIUS; ++y) {
        const int q = PUPIL_RADIUS * PUPIL_RADIUS - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(pupil_x + x, pupil_y + y, false);
    }
    fill_circle(pupil_x - 2, pupil_y - 3, 3);
}

static void draw_blink_eye(int cx, int cy)
{
    line(cx - 11, cy, cx - 5, cy + 1);
    line(cx - 5, cy + 1, cx + 5, cy + 1);
    line(cx + 5, cy + 1, cx + 11, cy);
}

static void draw_happy_eye(int cx, int cy)
{
    for (int x = -12; x <= 12; ++x) {
        const float t = (float)x / 12.0f;
        const int y = (int)(7.0f * (1.0f - t * t));
        pixel(cx + x, cy + y);
        if ((x & 1) == 0) pixel(cx + x, cy + y + 1);
    }
}

static void draw_normal_brow(int cx, int cy)
{
    static const int pts[][2] = {
        {-13, 2}, {-12, 1}, {-11, 0}, {-10, -1}, {-9, -2}, {-8, -3},
        {-7, -4}, {-5, -5}, {-3, -5}, {0, -5}, {3, -5}, {5, -5},
        {7, -4}, {8, -3}, {9, -2}, {10, -1}, {11, 0}, {12, 1}, {13, 2}
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); ++i) {
        pixel(cx + pts[i][0], cy + pts[i][1]);
        pixel(cx + pts[i][0], cy + pts[i][1] + 1);
    }
}

static void draw_sad_brow(int cx, int cy, bool left_eye)
{
    static const int left_pts[][2] = {
        {-13, -1}, {-12, 0}, {-11, 0}, {-10, 1}, {-9, 1}, {-8, 2},
        {-7, 2}, {-6, 3}, {-4, 4}, {-2, 5}, {0, 5}, {2, 6}, {4, 6},
        {6, 7}, {8, 7}, {10, 8}, {12, 8}, {13, 9}
    };
    static const int right_pts[][2] = {
        {-13, 9}, {-12, 8}, {-10, 8}, {-8, 7}, {-6, 7}, {-4, 6},
        {-2, 6}, {0, 5}, {2, 5}, {4, 4}, {6, 3}, {7, 2}, {8, 2},
        {9, 1}, {10, 1}, {11, 0}, {12, 0}, {13, -1}
    };
    const auto &pts = left_eye ? left_pts : right_pts;
    constexpr size_t count = sizeof(left_pts) / sizeof(left_pts[0]);
    for (size_t i = 0; i < count; ++i) {
        pixel(cx + pts[i][0], cy + pts[i][1]);
        pixel(cx + pts[i][0], cy + pts[i][1] + 1);
    }
}

static void draw_sad_eye(int cx, int cy, int gaze_y)
{
    const int y = cy + gaze_y;
    line(cx - 11, y, cx - 5, y + 2);
    line(cx - 5, y + 2, cx + 5, y + 2);
    line(cx + 5, y + 2, cx + 11, y);
}

static void draw_sleep_eye(int cx, int cy)
{
    for (int x = cx - 12; x <= cx + 12; ++x) {
        pixel(x, cy);
        pixel(x, cy + 1);
    }
}

static void draw_error_eye(int cx, int cy)
{
    constexpr int SIZE = 10;
    for (int i = -SIZE; i <= SIZE; ++i) {
        pixel(cx + i, cy + i);
        pixel(cx + i, cy + i + 1);
        pixel(cx + i, cy - i);
        pixel(cx + i, cy - i + 1);
    }
}

static void render_mochi_gaze(int expr, int step,
                              int sX, int sY,
                              int gaze_x, int gaze_y,
                              int eye_shift_x, int eye_shift_y)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
    const int left_x = 34 + sX;
    const int right_x = 94 + sX;
    const int eye_y = 28 + sY;

    if (expr != 99) {
        if (expr == 6) {
            draw_sad_brow(left_x + eye_shift_x, 14 + sY, true);
            draw_sad_brow(right_x + eye_shift_x, 14 + sY, false);
        } else {
            draw_normal_brow(left_x + eye_shift_x, 14 + sY);
            draw_normal_brow(right_x + eye_shift_x, 14 + sY);
        }
    }

    if (step == 3) {
        draw_sleep_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_sleep_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }
    if (expr == 2 && step == 2) {
        draw_happy_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_happy_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }
    if (expr == 6) {
        draw_sad_eye(left_x + eye_shift_x, eye_y + eye_shift_y, gaze_y);
        draw_sad_eye(right_x + eye_shift_x, eye_y + eye_shift_y, gaze_y);
        return;
    }
    if (expr == 99) {
        draw_error_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_error_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }
    if (step == 1) {
        draw_blink_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_blink_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }
    draw_open_eye(left_x, eye_y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
    draw_open_eye(right_x, eye_y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
}

static uint32_t elapsed_ms(uint32_t now_ms)
{
    return now_ms - s_state_started_ms;
}

static void animation_parameters(uint32_t now_ms,
                                 int &expr,
                                 int &step,
                                 int &gaze_x,
                                 int &gaze_y,
                                 int &eye_shift_x,
                                 int &eye_shift_y)
{
    expr = 0;
    step = 0;
    gaze_x = 0;
    gaze_y = 0;
    eye_shift_x = 0;
    eye_shift_y = 0;

    const uint32_t t = elapsed_ms(now_ms);

    switch (s_current_face_state) {
        case FACE_IDLE: {
            const uint32_t cycle = t % 4200;
            if (cycle < 500) gaze_x = -3;
            else if (cycle < 1000) gaze_x = 0;
            else if (cycle < 1500) gaze_x = 3;
            else if (cycle < 2000) gaze_x = 0;
            else if (cycle >= 3000 && cycle < 3120) step = 1;
            break;
        }
        case FACE_LISTENING: {
            expr = 1;
            const uint32_t cycle = t % 3600;
            if (cycle < 700) gaze_x = -2;
            else if (cycle < 1400) gaze_x = 0;
            else if (cycle < 2100) gaze_x = 2;
            else gaze_x = 0;
            if (cycle >= 2500 && cycle < 2620) step = 1;
            break;
        }
        case FACE_THINKING: {
            expr = 0;
            const uint32_t cycle = t % 4200;
            if (cycle < 1200) gaze_y = -5;
            else if (cycle < 2000) { gaze_y = -5; gaze_x = -3; }
            else if (cycle < 2800) { gaze_y = -5; gaze_x = 3; }
            else gaze_y = 0;
            if (cycle >= 3200 && cycle < 3320) step = 1;
            break;
        }
        case FACE_SPEAKING: {
            expr = 2;
            const uint32_t cycle = t % 1800;
            if (cycle < 450) gaze_x = 0;
            else if (cycle < 900) gaze_x = 2;
            else if (cycle < 1350) gaze_x = 0;
            else gaze_x = -2;
            if (cycle >= 1550 && cycle < 1670) step = 1;
            break;
        }
        case FACE_HAPPY: {
            expr = 2;
            const uint32_t cycle = t % 2600;
            if (cycle < 800) step = 2;
            else if (cycle < 1050) step = 1;
            else if (cycle < 1700) step = 2;
            else if (cycle < 2100) { step = 0; gaze_x = 3; }
            else step = 2;
            break;
        }
        case FACE_SAD: {
            expr = 6;
            const uint32_t cycle = t % 3200;
            if (cycle < 1000) gaze_y = 2;
            else if (cycle < 2000) gaze_y = 3;
            else gaze_y = 2;
            break;
        }
        case FACE_ERROR:
            expr = 99;
            break;
        case FACE_SLEEP:
            step = 3;
            break;
        default:
            break;
    }
}

static void render_current_face(uint32_t now_ms)
{
    int expr = 0;
    int step = 0;
    int gaze_x = 0;
    int gaze_y = 0;
    int eye_shift_x = 0;
    int eye_shift_y = 0;
    animation_parameters(now_ms, expr, step, gaze_x, gaze_y,
                         eye_shift_x, eye_shift_y);
    render_mochi_gaze(expr, step, 0, 0,
                      gaze_x, gaze_y, eye_shift_x, eye_shift_y);
}

} // namespace

void display_face_init(void)
{
    s_current_face_state = FACE_IDLE;
    s_previous_face_state = FACE_IDLE;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_state_started_ms = now_ms;
    s_override_until_ms = 0;
    s_override_active = false;
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
    render_current_face(now_ms);
}

void display_face_update(uint32_t now_ms)
{
    if (s_override_active && (int32_t)(now_ms - s_override_until_ms) >= 0) {
        s_override_active = false;
        s_current_face_state = s_previous_face_state;
        s_state_started_ms = now_ms;
    }
    render_current_face(now_ms);
}

void display_face_set_state(face_state_t state)
{
    if (state < FACE_IDLE || state > FACE_SLEEP) state = FACE_IDLE;
    s_override_active = false;
    s_current_face_state = state;
    s_previous_face_state = state;
    s_state_started_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void display_face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    if (state < FACE_IDLE || state > FACE_SLEEP) state = FACE_IDLE;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (duration_ms == 0) {
        display_face_set_state(state);
        return;
    }
    s_previous_face_state = s_current_face_state;
    s_current_face_state = state;
    s_state_started_ms = now_ms;
    s_override_until_ms = now_ms + duration_ms;
    s_override_active = true;
}

face_state_t display_face_get_state(void)
{
    return s_current_face_state;
}

void display_face_render_mochi_gaze(int expr, int step,
                                    int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y)
{
    render_mochi_gaze(expr, step, sX, sY,
                      gaze_x, gaze_y, eye_shift_x, eye_shift_y);
}

void display_face_render_mochi(int expr, int step,
                               int sX, int sY, int arahLirik)
{
    int gaze_x = 0;
    int gaze_y = 0;
    switch (arahLirik) {
        case 1: gaze_x = -5; break;
        case 2: gaze_x = 5; break;
        case 3: gaze_y = -5; break;
        default: break;
    }
    display_face_render_mochi_gaze(expr, step, sX, sY,
                                   gaze_x, gaze_y, 0, 0);
}

void display_face_render(void)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    render_current_face(now_ms);
}

const uint8_t *display_face_buffer(void)
{
    return s_face_buffer;
}
