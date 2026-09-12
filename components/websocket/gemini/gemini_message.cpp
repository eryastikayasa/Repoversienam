#include "gemini_message.h"
#include "cJSON.h"

#include <stdlib.h>

extern "C" gemini_message_type_t gemini_message_classify(const char *json, size_t len)
{
    if (!json || len == 0) return GEMINI_MESSAGE_UNKNOWN;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return GEMINI_MESSAGE_UNKNOWN;

    gemini_message_type_t type = GEMINI_MESSAGE_UNKNOWN;
    if (cJSON_GetObjectItemCaseSensitive(root, "error")) {
        type = GEMINI_MESSAGE_ERROR;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete")) {
        type = GEMINI_MESSAGE_SETUP;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "serverContent")) {
        type = GEMINI_MESSAGE_SERVER_CONTENT;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate")) {
        type = GEMINI_MESSAGE_SESSION_RESUMPTION;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "goAway")) {
        type = GEMINI_MESSAGE_GOAWAY;
    }

    cJSON_Delete(root);
    return type;
}
