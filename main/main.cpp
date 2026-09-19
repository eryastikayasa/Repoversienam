
    while (1) {
        if (websocket_standby_requested()) {
            /*
             * Standby is an isolated shutdown path. Do not disconnect merely
             * because the previous Gemini turn has already drained: after the
             * toolResponse, Gemini still needs time to generate the standby
             * acknowledgement. Shutdown starts only after that new response
             * actually begins and its playback has fully drained.
             */
            const bool response_started = websocket_standby_response_started();
            const bool response_drained = response_started &&
                                          !audio_turn_active &&
                                          !audio_turn_complete_pending;
            const bool response_timeout = websocket_standby_timeout_expired();

            if (assistant_active && (response_drained || response_timeout)) {
                ESP_LOGI(TAG,
                         "Standby Gemini: %s -> menutup sesi dan mengaktifkan Wake Word",
                         response_drained ? "respons audio selesai + drain" : "timeout respons");
                websocket_clear_standby_request();
                assistant_active = false;
                wait_for_gemini_mic_release();
                face_set_state(FACE_SLEEP);
                display_status("Katakan: Hi, ESP");
                websocket_disconnect();
                last_user_activity_us = 0;
                connect_start_us = 0;
            }
        }

        if (!assistant_active) {
            if (wakeword_detected()) {
                wakeword_clear_detected();
                wakeword_stop();
                (void)audio_hal_stop_capture();
                if (!afe_audio_init()) {
                    ESP_LOGE(TAG, "AFE init gagal; Gemini session tidak dimulai");
                    face_set_state(FACE_ERROR);
                    continue;