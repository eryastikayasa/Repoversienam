#include "display_text.h"

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>

namespace {

constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
constexpr uint32_t TEXT_SCROLL_STEP_MS = 33;

static EXT_RAM_BSS_ATTR uint8_t s_text_buffer[OLED_WIDTH * OLED_HEIGHT / 8] = {0};
static char s_user_scroll_text[256] = {0};
static char s_gemini_scroll_text[256] = {0};
static char s_status_text[64] = {0};
static uint16_t s_user_scroll_offset = 0;
static uint16_t s_gemini_scroll_offset = 0;
static uint32_t s_last_update_ms = 0;
static portMUX_TYPE s_scroll_text_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t text_glyph_row(char c, int row)
{
    static const uint8_t letters[26][5] = {
        {14,17,31,17,17}, {30,17,30,17,30}, {15,16,16,16,15},
        {30,17,17,17,30}, {31,16,30,16,31}, {31,16,30,16,16},
        {15,16,23,17,15}, {17,17,31,17,17}, {31,4,4,4,31},
        {7,2,2,18,12}, {17,18,28,18,17}, {16,16,16,16,31},
        {17,27,21,17,17}, {17,25,21,19,17}, {14,17,17,17,14},
        {30,17,30,16,16}, {14,17,21,19,15}, {30,17,30,18,17},
        {15,16,14,1,30}, {31,4,4,4,4}, {17,17,17,17,14},
        {17,17,17,10,4}, {17,17,21,27,17}, {17,10,4,10,17},
        {17,10,4,4,4}, {31,2,4,8,31}
    };
    static const uint8_t digits[10][5] = {
        {14,17,19,21,14}, {4,12,4,4,14}, {14,1,6,8,31},
        {30,1,6,1,30}, {18,18,31,2,2}, {31,16,30,1,30},
        {14,16,30,17,14}, {31,1,2,4,4}, {14,17,14,17,14},
        {14,17,15,1,14}
    };

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c >= '0' && c <= '9') return digits[c - '0'][row];

    switch (c) {
        case '-': return row == 2 ? 14 : 0;
        case '.': return row == 4 ? 4 : 0;
        case ',': return row == 4 ? 6 : 0;
        case '!': return row < 4 ? 4 : 0;
        case ':': return (row == 1 || row == 3) ? 4 : 0;
        case '?': return row == 0 ? 14 : row == 1 ? 1 : row == 2 ? 6 : row == 4 ? 4 : 0;
        case '/': return (uint8_t)(1U << (4 - row));
        case ' ': return 0;
        default: return 0;
    }
}

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t &b = s_text_buffer[x + (y >> 3) * OLED_WIDTH];
    uint8_t m = (uint8_t)(1U << (y & 7));
    if (on) b |= m;
    else b &= (uint8_t)~m;
}

static void draw_text_char(int x, int y, char c)
{
    for (int row = 0; row < 5; ++row) {
        uint8_t bits = text_glyph_row(c, row);
        for (int col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) pixel(x + col, y + row, true);
        }
    }
}

static void draw_scrolling_text(const char *text, uint16_t offset, int text_x)
{
    if (!text || !text[0]) return;

    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    constexpr int CHAR_WIDTH = 6;
    constexpr int TEXT_RIGHT = OLED_WIDTH - 1;
    constexpr int GAP_PX = 12;

    const size_t len = strlen(text);
    const int text_px = (int)len * CHAR_WIDTH;
    const int visible_width = TEXT_RIGHT - text_x + 1;

    if (visible_width <= 5) return;

    if (text_px <= visible_width) {
        for (size_t i = 0; i < len; ++i)
            draw_text_char(text_x + (int)i * CHAR_WIDTH, TEXT_Y, text[i]);
        return;
    }

    const int cycle_px = text_px + GAP_PX;
    int pos = TEXT_RIGHT + 1 - (int)offset;

    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;
        if (x + 5 >= text_x && x <= TEXT_RIGHT) draw_text_char(x, TEXT_Y, text[i]);
    }

    pos += cycle_px;
    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;
        if (x + 5 >= text_x && x <= TEXT_RIGHT) draw_text_char(x, TEXT_Y, text[i]);
    }
}

static void set_scroll_text(char *dst, size_t dst_size, const char *text, uint16_t &offset)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    offset = 0;
    if (!text) return;

    size_t out = 0;
    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E) dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void append_scroll_text(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0 || !text || !text[0]) return;

    size_t out = strlen(dst);
    if (out > 0 && out + 1 < dst_size) {
        dst[out++] = ' ';
        dst[out] = '\0';
    }

    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E) dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void draw_rssi_char(int x, int y, char c)
{
    static const uint8_t glyphs[][5] = {
        {0x00,0x00,0x1F,0x00,0x00},
        {0x1E,0x11,0x11,0x11,0x1E}, {0x00,0x12,0x1F,0x10,0x00},
        {0x12,0x19,0x15,0x13,0x12}, {0x11,0x15,0x15,0x15,0x0A},
        {0x07,0x04,0x04,0x1F,0x04}, {0x17,0x15,0x15,0x15,0x09},
        {0x0E,0x15,0x15,0x15,0x08}, {0x01,0x01,0x19,0x05,0x03},
        {0x0A,0x15,0x15,0x15,0x0A}, {0x02,0x15,0x15,0x15,0x0E},
    };

    int index = (c == '-') ? 0 : (c - '0' + 1);
    if (index < 0 || index >= (int)(sizeof(glyphs) / sizeof(glyphs[0]))) return;

    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyphs[index][col];
        for (int row = 0; row < 5; ++row) {
            if (bits & (1U << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT)
                    s_text_buffer[px + (py >> 3) * OLED_WIDTH] |= (uint8_t)(1U << (py & 7));
            }
        }
    }
}

static int draw_rssi(void)
{
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return 0;

    int rssi = ap_info.rssi;
    if (rssi > 0) rssi = 0;
    if (rssi < -99) rssi = -99;

    char text[5];
    snprintf(text, sizeof(text), "%d", rssi);

    const int char_width = 6;
    int x = 0;
    const int y = OLED_HEIGHT - 5;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        draw_rssi_char(x, y, text[i]);
        x += char_width;
    }
    return x;
}

static uint16_t advance_scroll_offset(const char *text, uint16_t offset, uint32_t steps)
{
    if (!text || !text[0]) return 0;

    constexpr uint16_t CHAR_WIDTH = 6;
    constexpr uint16_t GAP_PX = 12;
    const uint32_t text_px = (uint32_t)strlen(text) * CHAR_WIDTH;
    const uint32_t visible_width = OLED_WIDTH - 1 - 4 + 1;
    if (text_px <= visible_width) return 0;

    const uint32_t cycle_px = text_px + GAP_PX;
    return (uint16_t)((offset + steps) % cycle_px);
}

}

void display_text_init(void)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    s_user_scroll_text[0] = '\0';
    s_gemini_scroll_text[0] = '\0';
    s_status_text[0] = '\0';
    s_user_scroll_offset = 0;
    s_gemini_scroll_offset = 0;
    s_last_update_ms = 0;
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_update(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_scroll_text_mux);

    if (s_last_update_ms == 0) {
        s_last_update_ms = now_ms;
        portEXIT_CRITICAL(&s_scroll_text_mux);
        return;
    }

    const uint32_t elapsed_ms = now_ms - s_last_update_ms;
    const uint32_t steps = elapsed_ms / TEXT_SCROLL_STEP_MS;
    if (steps == 0) {
        portEXIT_CRITICAL(&s_scroll_text_mux);
        return;
    }

    s_user_scroll_offset = advance_scroll_offset(
        s_user_scroll_text, s_user_scroll_offset, steps);
    s_gemini_scroll_offset = advance_scroll_offset(
        s_gemini_scroll_text, s_gemini_scroll_offset, steps);

    s_last_update_ms += steps * TEXT_SCROLL_STEP_MS;
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_set_user(const char *text)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    set_scroll_text(s_user_scroll_text, sizeof(s_user_scroll_text), text, s_user_scroll_offset);
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_set_gemini(const char *text)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    set_scroll_text(s_gemini_scroll_text, sizeof(s_gemini_scroll_text), text, s_gemini_scroll_offset);
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_append_user(const char *text)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    append_scroll_text(s_user_scroll_text, sizeof(s_user_scroll_text), text);
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_append_gemini(const char *text)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    append_scroll_text(s_gemini_scroll_text, sizeof(s_gemini_scroll_text), text);
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_set_status(const char *text)
{
    portENTER_CRITICAL(&s_scroll_text_mux);
    set_scroll_text(s_status_text, sizeof(s_status_text), text, s_user_scroll_offset);
    portEXIT_CRITICAL(&s_scroll_text_mux);
}

void display_text_render_user(void)
{
    char text[256] = {0};
    uint16_t offset = 0;
    portENTER_CRITICAL(&s_scroll_text_mux);
    strncpy(text, s_user_scroll_text, sizeof(text) - 1);
    offset = s_user_scroll_offset;
    portEXIT_CRITICAL(&s_scroll_text_mux);

    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    const int rssi_end_x = draw_rssi();
    draw_scrolling_text(text, offset, rssi_end_x > 0 ? rssi_end_x + 4 : 4);
}

void display_text_render_gemini(void)
{
    char text[256] = {0};
    uint16_t offset = 0;
    portENTER_CRITICAL(&s_scroll_text_mux);
    strncpy(text, s_gemini_scroll_text, sizeof(text) - 1);
    offset = s_gemini_scroll_offset;
    portEXIT_CRITICAL(&s_scroll_text_mux);

    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    const int rssi_end_x = draw_rssi();
    draw_scrolling_text(text, offset, rssi_end_x > 0 ? rssi_end_x + 4 : 4);
}

void display_text_render_status(void)
{
    char text[64] = {0};
    portENTER_CRITICAL(&s_scroll_text_mux);
    strncpy(text, s_status_text, sizeof(text) - 1);
    portEXIT_CRITICAL(&s_scroll_text_mux);

    memset(s_text_buffer, 0, sizeof(s_text_buffer));

    constexpr int CHAR_WIDTH = 6;
    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    const size_t len = strlen(text);
    const int max_chars = OLED_WIDTH / CHAR_WIDTH;
    const int chars_to_draw = len < (size_t)max_chars ? (int)len : max_chars;

    int x = (OLED_WIDTH - chars_to_draw * CHAR_WIDTH) / 2;
    if (x < 0) x = 0;

    for (int i = 0; i < chars_to_draw; ++i) {
        draw_text_char(x + i * CHAR_WIDTH, TEXT_Y, text[i]);
    }
}

const uint8_t *display_text_buffer(void)
{
    return s_text_buffer;
}
