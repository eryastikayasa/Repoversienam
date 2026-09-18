
🏠 ESP32-S3 Asisten Kamar
Voice Commander berbasis ESP32-S3, terhubung ke ESP32 DevKit V1 sebagai IR Master & Relay, dengan Gemini Live API melalui WebSocket Secure.



🔌 Arsitektur Hardware
[ Mikrofon INMP441 ] ---> [ ESP32-S3 (Voice Commander) ]
                                │
                                │ UART / Serial1
                                ▼
                      [ ESP32 DevKit V1 (IR Master & Relay) ]
                                │
              ┌─────────────────┼─────────────────┬──────────┐
              ▼                 ▼                 ▼                      ▼
         [ Relay 4CH ]    [ IR Remote ]    [ DFPlayer                  / Mochi ]        [  Kipas ]
