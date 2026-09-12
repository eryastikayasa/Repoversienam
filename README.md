# RepoVersiEnam

# Arsitektur Audio + WebSocket

RepoVersiEnam dirancang dengan satu tujuan utama: **mudah diperbaiki, mudah diuji, dan mudah dikembangkan tanpa merusak modul lain**.

Dokumen ini menjadi **kontrak arsitektur** sebelum implementasi WebSocket dan integrasi Gemini dilanjutkan.

---

## 1. Alur Utama

```text
MIC
 │
 ▼
┌──────────────┐
│  AUDIO HAL   │  Hardware audio
└──────┬───────┘
       │ PCM
       ▼
┌──────────────┐
│ AUDIO ENGINE │  Pusat pipeline audio
└──────┬───────┘
       │ TX audio
       ▼
┌────────────────────┐
│     WEBSOCKET      │  Kurir / transport
└──────────┬─────────┘
           │
           ▼
        GEMINI
           │
           ▼
┌────────────────────┐
│     WEBSOCKET      │  Kurir / transport
└──────────┬─────────┘
           │ RX audio
           ▼
┌──────────────┐
│ AUDIO ENGINE │
└──────┬───────┘
       │ PCM
       ▼
┌──────────────┐
│  AUDIO HAL   │
└──────┬───────┘
       ▼
    SPEAKER
```

**WebSocket hanya kurir. Audio Engine adalah pusat audio. Audio HAL adalah hardware layer.**

---

## 2. Struktur Repository Target

```text
RepoVersiEnam/
│
├── main/
│   ├── main.cpp                 ← KOSONG
│   └── CMakeLists.txt
│
├── components/
│   │
│   ├── audio_hal/
│   │   ├── include/
│   │   │   └── audio_hal.h
│   │   └── audio_hal.cpp
│   │
│   ├── audio_engine/
│   │   ├── include/
│   │   │   └── audio_engine.h
│   │   ├── audio_engine.cpp
│   │   ├── audio_engine_mic.cpp
│   │   └── audio_engine_ingest.cpp
│   │
│   ├── websocket/
│   │   ├── include/
│   │   │   └── websocket.h
│   │   ├── websocket.cpp
│   │   └── websocket_gemini.cpp
│   │
│   ├── wifi_manager/
│   │   ├── include/
│   │   │   └── wifi_manager.h
│   │   └── wifi_manager.cpp
│   │
│   ├── uart_control/
│   │   ├── include/
│   │   │   └── uart_control.h
│   │   └── uart_control.cpp
│   │
│   ├── web_config/
│   │   ├── include/
│   │   │   └── web_config.h
│   │   └── web_config.cpp
│   │
│   └── display/
│       ├── display_driver/
│       ├── display_engine/
│       ├── display_face/
│       └── display_text/
│
├── partitions/
│   └── partitions.csv
│
├── platformio.ini
├── CMakeLists.txt
└── README.md
```

Struktur boleh bertambah jika kebutuhan teknis memerlukannya, tetapi **batas tanggung jawab modul tidak boleh kabur**.

---

## 3. Tanggung Jawab Setiap Layer

### Audio HAL

Audio HAL hanya menangani hardware audio:

- microphone
- speaker
- I2S
- DMA/I2S configuration
- sample rate hardware
- membaca microphone
- menulis speaker
- konversi format yang diperlukan hardware

Audio HAL **tidak mengetahui Gemini dan WebSocket**.

```text
MIC ──► Audio HAL ──► Audio Engine
Audio Engine ──► Audio HAL ──► SPEAKER
```

### Audio Engine

Audio Engine adalah **otak pipeline audio**.

Tanggung jawab:

- menerima audio dari Audio HAL
- mengelola buffer audio
- TX queue/buffer
- RX queue/buffer
- pemrosesan audio
- mengatur playback
- menerima audio dari jaringan melalui API WebSocket
- mengirim audio ke jaringan melalui API WebSocket
- menjaga state audio

Audio Engine **tidak mengetahui detail TCP/TLS/WebSocket/Gemini JSON**.

### WebSocket

WebSocket adalah **kurir**.

Tanggung jawab:

- membuat koneksi
- disconnect
- reconnect
- mengirim payload
- menerima payload
- event koneksi
- status koneksi
- transport WebSocket

WebSocket **tidak boleh**:

- membaca microphone secara langsung
- menulis speaker secara langsung
- mengakses I2S secara langsung
- memiliki buffer audio utama
- melakukan audio processing
- mengatur playback
- mengatur microphone
- mengambil alih lifecycle Audio Engine

---

## 4. Struktur Internal WebSocket

WebSocket sengaja dipisah menjadi transport dan adapter Gemini.

```text
components/websocket/
│
├── include/
│   └── websocket.h
│
├── websocket.cpp
│   │
│   ├── init
│   ├── connect
│   ├── disconnect
│   ├── send
│   ├── receive event
│   ├── connection state
│   └── reconnect
│
└── websocket_gemini.cpp
    │
    ├── Gemini session
    ├── Gemini message
    ├── Gemini JSON
    ├── Gemini audio packet
    └── Gemini protocol adapter
```

Tujuannya:

```text
Transport berubah
    → perbaiki websocket.cpp

Format Gemini berubah
    → perbaiki websocket_gemini.cpp

Audio pipeline berubah
    → perbaiki audio_engine

Hardware audio berubah
    → perbaiki audio_hal
```

Jangan membuat satu file WebSocket raksasa yang menangani semuanya.

---

## 5. Dependency Antar-Modul

Arah dependency yang diinginkan:

```text
                    ┌──────────────┐
                    │  AUDIO HAL   │
                    └──────▲───────┘
                           │
                           │ audio API
                           │
                    ┌──────┴───────┐
                    │ AUDIO ENGINE │
                    └──────▲───────┘
                           │
                           │ websocket API
                           │
                    ┌──────┴───────┐
                    │  WEBSOCKET   │
                    └──────────────┘
```

Yang **tidak boleh**:

```text
WebSocket ─────► Audio HAL
WebSocket ─────► I2S
WebSocket ─────► Speaker
WebSocket ─────► Microphone
```

WebSocket berkomunikasi dengan Audio Engine melalui API yang kecil dan stabil.

---

## 6. Kontrak API WebSocket

API publik awal dibuat sesederhana mungkin:

```cpp
void websocket_init(void);

bool websocket_connect(void);

bool websocket_send_audio(
    const uint8_t *data,
    size_t length
);

bool websocket_send_text(
    const char *text
);

void websocket_disconnect(void);

bool websocket_is_connected(void);
```

Untuk data masuk, gunakan salah satu mekanisme yang ditentukan saat implementasi:

```text
WebSocket
    │
    ├── callback
    │
    ├── event
    │
    └── queue
          │
          ▼
     Audio Engine
```

Pilihan final callback/event/queue akan ditentukan berdasarkan kebutuhan concurrency dan ownership buffer. **Tidak boleh membuat API yang mengikat WebSocket langsung ke Audio HAL.**

---

## 7. TX Pipeline

Microphone menuju Gemini:

```text
MIC
 │
 ▼
Audio HAL
 │
 ▼
Audio Engine
 │
 ├── capture
 ├── processing
 ├── TX buffer
 └── TX queue
 │
 ▼
WebSocket
 │
 ▼
Gemini
```

Audio Engine adalah pemilik buffer TX.

WebSocket hanya mengirim data yang diberikan kepadanya.

---

## 8. RX Pipeline

Gemini menuju speaker:

```text
Gemini
 │
 ▼
WebSocket
 │
 ├── receive
 └── validate/route protocol payload
 │
 ▼
Audio Engine
 │
 ├── RX queue
 ├── RX buffer
 ├── audio processing
 └── playback scheduling
 │
 ▼
Audio HAL
 │
 ▼
SPEAKER
```

WebSocket tidak boleh melewati Audio Engine untuk langsung menulis speaker.

---

## 9. Ownership Buffer

Aturan ownership harus jelas.

```text
Audio Engine
│
├── TX buffer  ← milik Audio Engine
├── TX queue   ← milik Audio Engine
├── RX buffer  ← milik Audio Engine
└── RX queue   ← milik Audio Engine

WebSocket
└── transport buffer sementara saja
```

WebSocket tidak boleh mengambil alih ownership buffer audio utama.

Ini penting supaya tidak terjadi:

- double free
- use-after-free
- buffer overwrite
- race condition
- memory leak

---

## 10. TX dan RX Harus Terpisah

Jangan membuat satu fungsi besar seperti:

```cpp
websocket_handle_audio_everything();
```

Sebaliknya:

```text
TX:
Audio Engine → WebSocket → Gemini

RX:
Gemini → WebSocket → Audio Engine
```

Keduanya memiliki buffer, counter, error, dan flow-control masing-masing.

---

## 11. State WebSocket

State koneksi tidak boleh dicampur dengan state audio.

```text
DISCONNECTED
      │
      ▼
 CONNECTING
      │
      ▼
 CONNECTED
      │
      ├───────────────┐
      ▼               │
    ERROR             │
      │               │
      ▼               │
  BACKOFF ────────────┘
```

Audio Engine cukup menerima informasi:

```text
CONNECTED
DISCONNECTED
ERROR
```

Audio Engine menentukan sendiri apa yang harus dilakukan terhadap pipeline audio.

---

## 12. Reconnect

Reconnect adalah tanggung jawab WebSocket.

```text
Connection Lost
      │
      ▼
WebSocket
      │
      ├── close/cleanup transport
      ├── backoff
      ├── reconnect
      └── connection event
                │
                ▼
          Audio Engine
```

WebSocket tidak boleh saat reconnect langsung mematikan atau menginisialisasi ulang I2S.

---

## 13. Gemini Protocol Adapter

Detail Gemini berada di `websocket_gemini.cpp`.

Audio Engine cukup mengetahui konsep:

```text
send audio
receive audio
connection status
session status
```

Audio Engine tidak perlu mengetahui:

- JSON Gemini
- field JSON Gemini
- URI Gemini
- TLS detail
- WebSocket opcode
- format event network
- authentication transport

Dengan demikian jika API/protokol Gemini berubah, dampaknya terlokalisasi.

---

## 14. Task dan Concurrency

Target awal:

```text
WebSocket task
└── transport/network

Audio Engine task
└── audio pipeline/playback

Audio HAL
└── hardware/DMA/I2S
```

Hindari desain seperti:

```text
WebSocket Task
├── network
├── JSON
├── microphone
├── I2S
├── speaker
├── audio processing
└── Gemini
```

Task harus mempunyai pekerjaan sempit.

Setiap queue/mutex harus mempunyai ownership yang jelas.

---

## 15. Error Handling

Error dipisahkan berdasarkan layer.

### Audio HAL

```text
I2S error
DMA error
MIC error
SPEAKER error
```

### Audio Engine

```text
TX overflow
RX overflow
underrun
buffer starvation
processing error
```

### WebSocket

```text
DNS error
TLS error
connection error
send error
receive error
protocol error
reconnect error
```

Error dari satu layer dikirim sebagai status/event. Jangan langsung memanggil hardware layer dari layer yang tidak berwenang.

---

## 16. Debugging Strategy

Arsitektur dibuat agar sumber masalah mudah dipersempit.

### MIC tidak menghasilkan data

```text
Audio HAL
```

### MIC normal, tetapi TX kosong

```text
Audio Engine
```

### TX keluar dari Audio Engine tetapi Gemini tidak menerima

```text
WebSocket
```

### Gemini mengirim data tetapi RX Audio Engine kosong

```text
WebSocket → Audio Engine interface
```

### RX Audio Engine ada tetapi speaker tidak bunyi

```text
Audio Engine → Audio HAL
```

### Speaker/I2S bermasalah

```text
Audio HAL
```

Dengan cara ini debugging tidak perlu membongkar seluruh firmware.

---

## 17. Aturan Keras Arsitektur

1. **WebSocket hanya kurir.**
2. **Audio Engine adalah pusat pipeline audio.**
3. **Audio HAL hanya menangani hardware audio.**
4. **WebSocket tidak mengakses I2S.**
5. **WebSocket tidak membaca microphone langsung.**
6. **WebSocket tidak menulis speaker langsung.**
7. **WebSocket tidak memiliki buffer audio utama.**
8. **Audio Engine memiliki ownership TX/RX audio buffer.**
9. **TX dan RX dipisahkan.**
10. **State koneksi tidak boleh menjadi state audio.**
11. **Reconnect menjadi tanggung jawab WebSocket.**
12. **Gemini protocol diisolasi dalam adapter WebSocket.**
13. **API antar-modul harus kecil dan stabil.**
14. **Tidak boleh ada task raksasa yang mengurus seluruh sistem.**
15. **`main.cpp` tetap kosong sesuai konsep RepoVersiEnam.**

---

## 18. Kontrak Akhir

```text
                  NETWORK SIDE
                       │
                       ▼
                ┌─────────────┐
                │  WEBSOCKET  │
                │   KURIR     │
                └──────┬──────┘
                       │
                       ▼
                    GEMINI
                       │
                       ▼
                ┌─────────────┐
                │  WEBSOCKET  │
                │   KURIR     │
                └──────┬──────┘
                       │
                       ▼
              ┌─────────────────┐
              │   AUDIO ENGINE  │
              │                 │
              │ TX │ RX │ BUF   │
              └───────┬─────────┘
                      │
                 audio API
                      │
                      ▼
              ┌───────────────┐
              │   AUDIO HAL   │
              │ I2S / MIC /  │
              │ SPEAKER       │
              └───────┬───────┘
                      │
             ┌────────┴────────┐
             ▼                 ▼
            MIC              SPEAKER
```

### Prinsip singkat

> **Audio HAL = hardware.**  
> **Audio Engine = otak audio.**  
> **WebSocket = kurir.**  
> **Gemini = server AI.**

Dokumen ini menjadi patokan implementasi. Jika implementasi berikutnya bertentangan dengan aturan di atas, **arsitektur harus diperbaiki terlebih dahulu sebelum menambah fitur**.

---

## Referensi WebSocket

RepoVersiEnam menggunakan `esp_websocket_client` sebagai implementasi transport WebSocket. Komponen tersebut mendukung koneksi WebSocket dan pengiriman data binary/text pada ESP-IDF. Versi dependency akan mengikuti konfigurasi project, bukan ditanamkan ke Audio Engine.
