#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>

#define MIC_I2S_SCK   GPIO_NUM_5
#define MIC_I2S_WS    GPIO_NUM_4
#define MIC_I2S_SD    GPIO_NUM_6
#define SPK_I2S_BCLK  GPIO_NUM_15
#define SPK_I2S_LRCK  GPIO_NUM_16
#define SPK_I2S_DOUT  GPIO_NUM_7
#define MIC_SAMPLE_RATE 16000
#define SPK_SAMPLE_RATE 24000
#define AUDIO_BITS 16
void audio_hal_init(void);
void audio_hal_ns_init(void);
void audio_i2s_test_tone(void);
size_t audio_read_mic(uint8_t *dest, size_t max_len);
size_t audio_write_speaker(const uint8_t *src, size_t len);
