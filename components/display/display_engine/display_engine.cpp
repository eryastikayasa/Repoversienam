#include "display_engine.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "display_face.h"
#include "display_text.h"
#include "display_driver.h"

namespace {

constexpr int DISPLAY_ENGINE_FRAME_MS = 33;
constexpr int DISPLAY_ENGINE_WIDTH = DISPLAY_DRIVER_WIDTH;
constexpr int DISPLAY_ENGINE_HEIGHT = DISPLAY_DRIVER_HEIGHT;
constexpr size_t DISPLAY_FRAMEBUFFER_SIZE =
    (size_t)DISPLAY_ENGINE_WIDTH * (size_t)DISPLAY_ENGINE_HEIGHT / 8U;

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

    for (size_t i = 0; i < sizeof(s_final_buffer); ++i) {
        s_final_buffer[i] |= text[i];
    }
}

static void update_text_layer(void)
{
    switch (display_face_get_state()) {
        case FACE_LISTENING:
            display_text_render_user();
            break;

        case FACE_SPEAKING:
            display_text_render_gemini();
            break;

        default:
            display_text_render_status();
            break;
    }
}

static void compose_frame(void)
{
    const uint8_t *face = display_face_buffer();
    const uint8_t *text = display_text_buffer();

    clear_final_frame();

    if (face) {
        memcpy(s_final_buffer, face, sizeof(s_final_buffer));
    }

    if (text) {
        overlay_text_buffer();
    }
}

static void display_engine_task(void *)
{
    TickType_t last_wake = xTaskGetTickCount();

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
    if (!s_initialized) {
        display_engine_init();
    }

    if (s_running || s_display_engine_task) return;

    s_running = true;

    BaseType_t result = xTaskCreate(
        display_engine_task,
        "display_engine",
        4096,
        nullptr,
        5,
        &s_display_engine_task
    );

    if (result != pdPASS) {
        s_running = false;
        s_display_engine_task = nullptr;
    }
}

void display_engine_stop(void)
{
    s_running = false;
}
