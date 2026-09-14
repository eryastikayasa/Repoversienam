#include "gemini_message.h"
#include "gemini_tool.h"
#include "cJSON.h"

extern "C" gemini_message_type_t gemini_message_classify_root(const cJSON *root)
{
    if (!root) return GEMINI_MESSAGE_UNKNOWN;

    if (cJSON_GetObjectItemCaseSensitive(root, "error")) return GEMINI_MESSAGE_ERROR;
    if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete")) return GEMINI_MESSAGE_SETUP;

    cJSON *tool_call = cJSON_GetObjectItemCaseSensitive(root, "toolCall");
    if (cJSON_IsObject(tool_call)) {
        // Only queue/copy small tool metadata here. UART and network response
        // work runs in the dedicated gemini_tool worker, never in ws_rx.
        gemini_tool_handle_call(tool_call);
        return GEMINI_MESSAGE_TOOL_CALL;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "serverContent")) return GEMINI_MESSAGE_SERVER_CONTENT;
    if (cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate")) return GEMINI_MESSAGE_SESSION_RESUMPTION;
    if (cJSON_GetObjectItemCaseSensitive(root, "goAway")) return GEMINI_MESSAGE_GOAWAY;
    return GEMINI_MESSAGE_UNKNOWN;
}

extern "C" gemini_message_type_t gemini_message_classify(const char *json, size_t len)
{
    if (!json || len == 0) return GEMINI_MESSAGE_UNKNOWN;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return GEMINI_MESSAGE_UNKNOWN;
    const gemini_message_type_t type = gemini_message_classify_root(root);
    cJSON_Delete(root);
    return type;
}
