# RepoVersiEnam

## Arsitektur Sistem ESP32-S3

```text
                 BOOT ESP32-S3
                       │
                       ▼
                ┌─────────────┐
                │  Web Config │
                └──────┬──────┘
                       │
                simpan / baca NVS
                       │
          ┌────────────┼────────────┐
          ▼            ▼            ▼
        WiFi        API Key        Role
          │            │            │
          └────────────┼────────────┘
                       ▼
                 MASUK SISTEM
                       │
                       ▼
                  Wake Word
                       │
                 "Hi ESP" aktif
                       │
                       ▼
                      MIC
                       │
                       ▼
                   Audio HAL
                       │
                       ▼
                 Audio Engine
                       │
                       ▼
                  WebSocket
                   (KURIR)
                       │
                       ▼
                    Gemini
                       │
                 audio response
                       │
                       ▼
                  WebSocket
                   (KURIR)
                       │
                       ▼
                 Audio Engine
                       │
                       ▼
                   Audio HAL
                       │
                       ▼
                   SPEAKER
```

### Urutan startup

1. ESP32-S3 boot.
2. NVS diinisialisasi untuk WiFi, API Key, dan Role.
3. Display dan UART diinisialisasi.
4. Jika konfigurasi belum lengkap, Web Config dijalankan dan konfigurasi disimpan ke NVS.
5. Audio HAL dan Audio Engine diinisialisasi.
6. WiFi tersambung dan menunggu IP.
7. WakeNet `wn9_hiesp` diaktifkan untuk Wake Word **Hi ESP**.
8. Wake Word menjadi gerbang sebelum sesi Gemini.
9. Setelah Wake Word terdeteksi, WebSocket membuka sesi Gemini.
10. Audio Engine mengatur aliran audio TX dan RX.

`app_main()` berada di `components/app_startup/app_startup.cpp`; `main/main.cpp` tetap kosong.

## Pipeline Audio

### TX

```text
MIC → Audio HAL → Audio Engine → WebSocket (kurir) → Gemini
```

### RX

```text
Gemini → WebSocket (kurir) → Audio Engine → Audio HAL → SPEAKER
```

## Pembagian Tanggung Jawab

**Audio HAL** = microphone, speaker, I2S, DMA, sample rate, dan konversi hardware.

**Audio Engine** = pusat pipeline audio, TX/RX buffer dan queue, processing, playback scheduling, serta state audio.

**Wake Word** = menerima frame PCM melalui Audio Engine dan mendeteksi `Hi ESP`; tidak membaca I2S langsung.

**WebSocket** = transport saja: connect, disconnect, reconnect, send, receive, dan event koneksi. WebSocket tidak boleh membaca microphone, menulis speaker, mengakses I2S, atau memiliki buffer audio utama.

**Gemini adapter** = detail session dan JSON Gemini berada di `components/websocket/websocket_gemini.cpp`.

## Ownership Buffer

```text
Audio Engine
├── TX buffer
├── TX queue
├── RX buffer
└── RX queue

WebSocket
└── transport buffer sementara
```

WebSocket tidak mengambil ownership buffer audio utama.

## Aturan Keras

1. WebSocket hanya kurir.
2. Audio Engine adalah pusat pipeline audio.
3. Audio HAL hanya hardware audio.
4. WebSocket tidak mengakses I2S.
5. WebSocket tidak membaca microphone langsung.
6. WebSocket tidak menulis speaker langsung.
7. WebSocket tidak memiliki buffer audio utama.
8. TX dan RX dipisahkan.
9. Reconnect adalah tanggung jawab WebSocket.
10. Detail protokol Gemini diisolasi di adapter Gemini.
11. Wake Word menerima audio melalui Audio Engine.
12. Wake Word menjadi gerbang sebelum sesi Gemini.
13. Tidak boleh ada task raksasa yang mengurus seluruh sistem.
14. `main.cpp` tetap kosong.

## Struktur Utama

```text
RepoVersiEnam/
├── main/
│   ├── main.cpp                 ← KOSONG
│   └── CMakeLists.txt
├── components/
│   ├── app_startup/             ← app_main()
│   ├── audio_hal/               ← HAL + Audio Engine
│   ├── wakeword/                ← WakeNet Hi ESP
│   ├── websocket/               ← transport + Gemini adapter
│   ├── wifi_manager/
│   ├── uart_control/
│   ├── web_config/
│   └── display/
├── partitions/
├── platformio.ini
├── CMakeLists.txt
└── README.md
```

### Prinsip singkat

> **Audio HAL = hardware.**  
> **Audio Engine = otak audio.**  
> **Wake Word = gerbang suara.**  
> **WebSocket = kurir.**  
> **Gemini = server AI.**
