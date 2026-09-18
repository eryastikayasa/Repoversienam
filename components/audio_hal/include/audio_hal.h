#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// ================================
// INMP441 - MICROPHONE
// ================================
#define MIC_I2S_SCK   GPIO_NUM_5
#define MIC_I2S_WS    GPIO_NUM_4
#define MIC_I2S_SD    GPIO_NUM_6

// ================================
// MAX98357A - SPEAKER
// ================================
#define SPK_I2S_BCLK  GPIO_NUM_15
#define SPK_I2S_LRCK  GPIO_NUM_16
#define SPK_I2S_DOUT  GPIO_NUM_7

// Gemini Live API input audio
#define MIC_SAMPLE_RATE 16000

// Gemini Live API native audio output
#define SPK_SAMPLE_RATE 24000

#define AUDIO_BITS 16

void audio_hal_init(void);

esp_err_t audio_hal_start_capture(void);
esp_err_t audio_hal_stop_capture(void);
esp_err_t audio_hal_read_pcm(int16_t *buffer, size_t samples, size_t *samples_read);

void audio_i2s_test_tone(void);

size_t audio_read_mic(uint8_t *dest, size_t max_len);

void audio_write_speaker(const uint8_t *src, size_t len);
