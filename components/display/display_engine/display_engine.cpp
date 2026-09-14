#include "display_engine.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display_face.h"
#include "display_text.h"
#include "display_driver.h"

namespace {

// Keep the existing single display task. 20 FPS is a conservative cadence for
// smoother face animation without unnecessarily increasing OLED/I2C pressure.
constexpr int DISPLAY_ENGINE_FRAME_MS = 50;
constexpr int DISPLAY_ENGINE_WIDTH = DISPLAY_DRIVER_WIDTH;
constexpr int DISPLAY_ENGINE_HEIGHT = DISPLAY_DRIVER_HEIGHT;
constexpr size_t DISPLAY_FRAMEBUFFER_SIZE =
    (size_t)DISPLAY_ENGINE_WIDTH * (size_t)DISPLAY_ENGINE_HEIGHT / 8U;
constexpr uint32_t DISPLAY_ENGINE_STACK = 4096U;
constexpr UBaseType_t DISPLAY_ENGINE_PRIORITY = 3U;
constexpr BaseType_t DISPLAY_ENGINE_CORE = 1;

static const char *TAG = "DISPLAY_ENGINE";
static EXT_RAM_BSS_ATTR uint8_t s_final_buffer[DISPLAY_FRAMEBUFFER_SIZE] = {0};
static TaskHandle_t s_display_engine_task = nullptr;
static bool s_initialized = false;
static volatile bool s_running = false;

static void clear_final_frame(void)
{
    memset(s_final_buffer, 0, sizeof(s_final_buffer));
}

static void overlay_text_buffer(void)
{
    const uint8_t *text = display_text_buffer();
    if (!text) return;
    for (size_t i = 0; i < sizeof(s_final_buffer); ++i) s_final_buffer[i] |= text[i];
}

static void update_text_layer(void)
{
    const face_state_t state = display_face_get_state();

    // Transcript ownership is separate from face/status state. A state change
    // must never manufacture or clear conversation text.
    switch (state) {
        case FACE_LISTENING:
            if (display_text_has_user()) display_text_render_user();
            else display_text_render_status();
            break;

        case FACE_THINKING:
            if (display_text_has_user()) display_text_render_user();
            else display_text_render_status();
            break;

        case FACE_SPEAKING:
            // Do not show a locally invented speaking string while output
            // transcription is still pending. Keep the user transcript visible
            // until the first real Gemini transcript arrives.
            if (display_text_has_gemini()) display_text_render_gemini();
            else if (display_text_has_user()) display_text_render_user();
            else display_text_render_status();
            break;

        default:
            if (display_text_has_user()) display_text_render_user();
            else if (display_text_has_gemini()) display_text_render_gemini();
            else display_text_render_status();
            break;
    }
}

static void compose_frame(void)
{
    const uint8_t *face = display_face_buffer();
    const uint8_t *text = display_text_buffer();
    clear_final_frame();
    if (face) memcpy(s_final_buffer, face, sizeof(s_final_buffer));
    if (text) overlay_text_buffer();
}

static void log_display_audit(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_display_engine_task) {
        ESP_LOGI(TAG,
                 "TASK AUDIT display_engine stack=%uB watermark=%uB priority=%u core=%d",
                 (unsigned)DISPLAY_ENGINE_STACK,
                 (unsigned)(uxTaskGetStackHighWaterMark(s_display_engine_task) * sizeof(StackType_t)),
                 (unsigned)uxTaskPriorityGet(s_display_engine_task),
                 (int)xTaskGetCoreID(s_display_engine_task));
    }

    ESP_LOGI(TAG,
             "RAM AUDIT[%s] internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u framebuffer=%uB PSRAM",
             stage ? stage : "unknown",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)DISPLAY_FRAMEBUFFER_SIZE);
}

static void display_engine_task(void *)
{
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_audit_us = 0;

    ESP_LOGI(TAG,
             "Display task: %dx%d framebuffer=%uB PSRAM frame=%ums priority=%u core=%d",
             DISPLAY_ENGINE_WIDTH,
             DISPLAY_ENGINE_HEIGHT,
             (unsigned)DISPLAY_FRAMEBUFFER_SIZE,
             (unsigned)DISPLAY_ENGINE_FRAME_MS,
             (unsigned)DISPLAY_ENGINE_PRIORITY,
             (int)DISPLAY_ENGINE_CORE);

    while (s_running) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        display_face_update(now_ms);
        display_text_update(now_ms);
        update_text_layer();
        compose_frame();

        display_driver_present(
            s_final_buffer,
            DISPLAY_ENGINE_WIDTH,
            DISPLAY_ENGINE_HEIGHT
        );

        const int64_t now_us = esp_timer_get_time();
        if (!last_audit_us || now_us - last_audit_us >= 10000000LL) {
            last_audit_us = now_us;
            log_display_audit("runtime");
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DISPLAY_ENGINE_FRAME_MS));
    }

    s_display_engine_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

void display_engine_init(void)
{
    if (s_initialized) return;
    clear_final_frame();
    display_driver_init();
    s_initialized = true;
}

void display_engine_start(void)
{
    if (!s_initialized) display_engine_init();
    if (s_running || s_display_engine_task) return;

    s_running = true;
    BaseType_t result = xTaskCreatePinnedToCore(
        display_engine_task,
        "display_engine",
        DISPLAY_ENGINE_STACK,
        nullptr,
        DISPLAY_ENGINE_PRIORITY,
        &s_display_engine_task,
        DISPLAY_ENGINE_CORE
    );

    if (result != pdPASS) {
        s_running = false;
        s_display_engine_task = nullptr;
        ESP_LOGE(TAG, "Display task create gagal");
    }
}

void display_engine_stop(void)
{
    s_running = false;
}

void display_set_system_state(face_state_t face, const char *status)
{
    // Status is UI metadata only. Conversation text is populated exclusively
    // by Gemini input/output transcription handlers and is intentionally left
    // untouched here.
    display_face_set_state(face);
    display_text_set_status(status ? status : "");
}
