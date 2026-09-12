#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AUDIO_ENGINE_IDLE = 0, AUDIO_ENGINE_LISTENING, AUDIO_ENGINE_THINKING, AUDIO_ENGINE_BUFFERING, AUDIO_ENGINE_PLAYING, AUDIO_ENGINE_PLAYING_LOW, AUDIO_ENGINE_DRAINING, AUDIO_ENGINE_COMPLETE, AUDIO_ENGINE_INTERRUPTED, AUDIO_ENGINE_ERROR } audio_engine_state_t;
typedef enum { AUDIO_ENGINE_EVENT_NONE = 0, AUDIO_ENGINE_EVENT_MODEL_BEGIN, AUDIO_ENGINE_EVENT_MODEL_AUDIO, AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE, AUDIO_ENGINE_EVENT_INTERRUPT, AUDIO_ENGINE_EVENT_GENERATION_CHANGED, AUDIO_ENGINE_EVENT_ERROR } audio_engine_event_type_t;
typedef struct { uint32_t generation; bool model_started; bool model_complete; bool playback_started; bool playback_drained; uint64_t bytes_received; uint64_t bytes_queued; uint64_t bytes_played; uint64_t network_drop; uint64_t playback_drop; uint32_t underrun_count; size_t pending_bytes; } audio_engine_turn_t;
typedef void (*audio_engine_mic_frame_cb_t)(const uint8_t *pcm, size_t len, void *ctx);
typedef void (*audio_engine_mic_sink_cb_t)(const uint8_t *pcm, size_t len, void *ctx);
bool audio_engine_init(void); audio_engine_state_t audio_engine_get_state(void); const char *audio_engine_state_name(audio_engine_state_t state); bool audio_engine_turn_active(void); const audio_engine_turn_t *audio_engine_get_turn(void); void audio_engine_notify(audio_engine_event_type_t event, uint32_t generation);
bool audio_engine_push_model_audio(const uint8_t *pcm, size_t len, uint32_t generation); bool audio_engine_push_model_audio_base64(const char *b64, size_t len, uint32_t generation);
bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb, void *ctx); bool audio_engine_set_mic_sink(audio_engine_mic_sink_cb_t cb, void *ctx); bool audio_engine_start_capture(void); void audio_engine_start_input_session(void); void audio_engine_stop_input_session(void); bool audio_engine_input_session_active(void);
#ifdef __cplusplus
}
#endif
