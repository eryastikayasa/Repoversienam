#pragma once

#include "display_face.h"

#ifdef __cplusplus
extern "C" {
#endif

void display_engine_init(void);
void display_engine_start(void);
void display_engine_stop(void);

/* Single lightweight presentation entry point for system state. */
void display_set_system_state(face_state_t face, const char *status);

#ifdef __cplusplus
}
#endif
