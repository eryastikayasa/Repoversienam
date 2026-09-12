#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// XiaoZhi 128x64 SSD1306 hardware configuration.
#define DISPLAY_DRIVER_SDA_PIN   41
#define DISPLAY_DRIVER_SCL_PIN   42
#define DISPLAY_DRIVER_I2C_ADDR  0x3C
#define DISPLAY_DRIVER_WIDTH     128
#define DISPLAY_DRIVER_HEIGHT    64
#define DISPLAY_DRIVER_I2C_HZ    400000

// Initialize the physical SSD1306 display and its I2C bus.
void display_driver_init(void);

// Send one complete 1-bit framebuffer to the SSD1306.
void display_driver_present(const uint8_t *buffer, int width, int height);

// Returns true after successful hardware initialization.
bool display_driver_is_ready(void);

#ifdef __cplusplus
}
#endif
