#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_DRIVER_SDA_PIN   41
#define DISPLAY_DRIVER_SCL_PIN   42
#define DISPLAY_DRIVER_I2C_ADDR  0x3C
#define DISPLAY_DRIVER_WIDTH     128
#define DISPLAY_DRIVER_HEIGHT    64
#define DISPLAY_DRIVER_I2C_HZ    400000

void display_driver_init(void);
void display_driver_present(const uint8_t *buffer, int width, int height);
bool display_driver_is_ready(void);

#ifdef __cplusplus
}
#endif
