#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool afe_audio_init(void);
void afe_audio_deinit(void);
bool afe_audio_is_ready(void);

/*
 * Feed one 16 kHz mono PCM16 frame into ESP-SR AFE and, when a processed
 * frame is ready, copy it to output. The AFE itself performs NS; no local
 * amplitude/VAD gate is applied here.
 */
bool afe_audio_process(const int16_t *input, size_t input_samples,
                       int16_t *output, size_t output_capacity_samples,
                       size_t *output_samples);
/* Drain one already-processed AFE output frame without feeding new input. */
bool afe_audio_fetch_output(int16_t *output, size_t output_capacity_samples,
                            size_t *output_samples);
/* Discard all already-processed AFE output frames. */
void afe_audio_flush_output(void);

int afe_audio_get_feed_samples(void);
int afe_audio_get_fetch_samples(void);
int afe_audio_get_sample_rate(void);

#ifdef __cplusplus
}
#endif
