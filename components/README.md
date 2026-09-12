# Component Architecture

RepoVersiEnam memisahkan firmware menjadi lapisan:

- `hal/` — akses hardware murni (audio, display, buttons, storage)
- `services/` — Wi-Fi, time sync, WebSocket/network, configuration
- `app/` — state machine, session orchestration, lifecycle
- `common/` — event types, logging helpers, shared constants

Aturan dependency:

`main -> app -> services/hal -> ESP-IDF`

HAL tidak boleh memanggil application state. Network service tidak boleh mengatur task audio secara langsung. Application layer menjadi pemilik lifecycle sesi.
