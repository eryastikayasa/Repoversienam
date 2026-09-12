#include "websocket.h"

/*
 * Repo6 transport implementation boundary.
 *
 * Keep this file responsible only for WebSocket lifecycle and transport.
 * Gemini message/protocol handling belongs in websocket_gemini.cpp.
 * Audio Engine remains the owner of audio flow and buffers.
 */

void websocket_init(void)
{
    /* Implementation comes in the next WebSocket step. */
}

bool websocket_connect(void)
{
    return false;
}

void websocket_disconnect(void)
{
}

bool websocket_is_connected(void)
{
    return false;
}

bool websocket_send_audio(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;
    return false;
}

bool websocket_send_text(const char *text)
{
    (void)text;
    return false;
}
