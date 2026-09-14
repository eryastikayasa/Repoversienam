cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Repo6 uses esp_websocket_client v1.7.0. Its send path treats a transport
# write timeout (return 0, no errno) as a fatal transport error and aborts the
# WebSocket. That makes a bounded 20 ms MIC send budget self-defeating.
# Patch only that exact upstream condition at configure time so a finite MIC
# send timeout returns 0 to Repo6 without aborting the connection. Real
# transport errors (negative return) remain fatal.
file(GLOB_RECURSE REPO6_WS_CLIENT_SOURCES
     "${CMAKE_SOURCE_DIR}/managed_components/*/esp_websocket_client.c")
foreach(REPO6_WS_CLIENT_SOURCE IN LISTS REPO6_WS_CLIENT_SOURCES)
    file(READ "${REPO6_WS_CLIENT_SOURCE}" REPO6_WS_CLIENT_TEXT)
    string(FIND "${REPO6_WS_CLIENT_TEXT}" "if (wlen == 0 && need_write != 0 && timeout != portMAX_DELAY)" REPO6_WS_ALREADY_PATCHED)
    if(REPO6_WS_ALREADY_PATCHED EQUAL -1)
        set(REPO6_WS_OLD "        if (wlen < 0 || (wlen == 0 && need_write != 0)) {")
        set(REPO6_WS_NEW "        if (wlen == 0 && need_write != 0 && timeout != portMAX_DELAY) {\n            ret = 0;\n            ESP_LOGW(TAG, \"Transport write timeout; preserving WebSocket connection\");\n            esp_websocket_free_buf(client, true);\n            goto unlock_and_return;\n        }\n        if (wlen < 0 || (wlen == 0 && need_write != 0)) {")
        string(FIND "${REPO6_WS_CLIENT_TEXT}" "${REPO6_WS_OLD}" REPO6_WS_OLD_POS)
        if(REPO6_WS_OLD_POS EQUAL -1)
            message(FATAL_ERROR "Repo6: esp_websocket_client.c found, but expected v1.7.0 send condition was not found: ${REPO6_WS_CLIENT_SOURCE}")
        endif()
        string(REPLACE "${REPO6_WS_OLD}" "${REPO6_WS_NEW}" REPO6_WS_CLIENT_TEXT "${REPO6_WS_CLIENT_TEXT}")
        file(WRITE "${REPO6_WS_CLIENT_SOURCE}" "${REPO6_WS_CLIENT_TEXT}")
        message(STATUS "Repo6: patched esp_websocket_client finite send timeout: ${REPO6_WS_CLIENT_SOURCE}")
    else()
        message(STATUS "Repo6: esp_websocket_client timeout patch already present: ${REPO6_WS_CLIENT_SOURCE}")
    endif()
endforeach()

project(Repoversienam)
