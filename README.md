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


## Struktur Utama

```text
Repoversienam/
│
├── main/
│   └── main.cpp
│
├── components/
│   ├── app_startup/
│   │
│   ├── audio_hal/
│   │   ├── audio_hal.cpp
│   │   ├── audio_engine.cpp
│   │   ├── audio_engine_ingest.cpp
│   │   ├── audio_engine_mic.cpp
│   │   └── include/
│   │
│   ├── websocket/
│   │   ├── websocket.cpp
│   │   ├── websocket_audio.cpp
│   │   ├── websocket_event.cpp
│   │   ├── websocket_transport.cpp
│   │   ├── include/
│   │   └── gemini/
│   │       ├── gemini_protocol.cpp
│   │       ├── gemini_message.cpp
│   │       └── gemini_audio.cpp
│   │
│   ├── wakeword/
│   │
│   ├── display/
│   │   ├── display_driver/
│   │   ├── display_engine/
│   │   ├── display_face/
│   │   └── display_text/
│   │
│   └── uart_control/
│
├── CMakeLists.txt
├── README.md
└── .github/workflows/build.yml
```

### Prinsip singkat

> **Audio HAL = hardware.**  
> **Audio Engine = otak audio.**  
> **Wake Word = gerbang suara.**  
> **WebSocket = kurir.**  
> **Gemini = server AI.**

1. Buffer input jangan ditahan terlalu lama
Google menyarankan jangan men-buffer audio input sampai sekitar 1 detik sebelum dikirim. Untuk real-time, audio harus dialirkan terus. �
Google AI for Developers
Jadi arsitektur kita:
MIC
 ↓
Audio HAL
 ↓
PCM16 16kHz mono
 ↓
Audio Engine
 ↓
buffer/queue kecil
 ↓
WebSocket
 ↓
Gemini
Audio Engine boleh buffering, tetapi tujuannya untuk smoothing/queue, bukan mengumpulkan 1 detik audio baru kemudian mengirim.
2. Ukuran chunk ada panduan resmi
Dokumentasi best-practice terbaru Google menyarankan 20–40 ms per chunk untuk streaming real-time. �
Google AI for Developers
Pada 16 kHz:
Durasi
Sample
PCM16
20 ms
320
640 byte
30 ms
480
960 byte
40 ms
640
1280 byte
Repo kita sekarang memakai 320 sample / 20 ms = 640 byte pada frame Audio Engine. Itu malah pas dengan rekomendasi ini.
Jadi tidak perlu mengubah menjadi 100 ms hanya karena ada halaman dokumentasi tertentu yang menggunakan contoh 100 ms. Best-practice Live API yang lebih umum sekarang menyebut 20–40 ms. �
Google AI for Developers +1
3. Buffer output Gemini juga harus diperlakukan sebagai stream
Gemini mengirim audio response sebagai potongan/chunks, bukan satu file audio utuh. �
Google AI for Developers
Maka:
Gemini
 ↓
WebSocket
 ↓
Base64 decode
 ↓
PCM16 24kHz
 ↓
Audio Engine RX buffer
 ↓
Playback queue
 ↓
Audio HAL
 ↓
I2S
 ↓
Speaker
Jangan:
Gemini → kumpulkan seluruh jawaban → baru play
Tetapi:
Gemini chunk 1 → queue → play
Gemini chunk 2 → queue → play
Gemini chunk 3 → queue → play
...
Ini yang cocok untuk low latency.
4. Aturan paling penting: interrupted
Ini sangat penting untuk kode kita.
Google secara eksplisit mengatakan ketika user mulai bicara saat Gemini sedang menjawab, server bisa mengirim:
"interrupted": true
Client harus segera membuang audio yang masih berada di buffer client supaya Gemini tidak terus berbicara setelah user mengambil giliran. �
Google AI for Developers
Jadi seharusnya:
Gemini sedang bicara
        ↓
Audio Engine punya PCM di queue
        ↓
Gemini → interrupted=true
        ↓
Audio Engine FLUSH
        ↓
buffer PCM dibuang
        ↓
speaker berhenti
        ↓
MIC lanjut
Ini bukan sekadar optimasi. Ini bagian penting dari implementasi Live API.
5. PCM tidak boleh berubah di tengah pipeline
Kontrak kita sebaiknya dibuat sangat ketat:
MIC → Gemini
PCM16
signed
little-endian
mono
16000 Hz
raw PCM
Google memang menetapkan raw PCM 16-bit little-endian dan native input 16 kHz. MIME harus memberi tahu sample rate. �
Google AI for Developers +1
audio/pcm;rate=16000
Gemini → Speaker
PCM16
signed
little-endian
24 kHz
Output Live API menggunakan PCM16 24 kHz. �
Google AI for Developers
Jadi I2S kita boleh menggunakan 32-bit hardware format, tetapi:
Gemini PCM16
     ↓
Audio Engine PCM16
     ↓
Audio HAL
     ↓
PCM16 → PCM32
     ↓
I2S 32-bit
Konversi 32-bit hanya terjadi di boundary hardware, bukan di WebSocket.
6. Base64 bukan format audio
Ini juga perlu kita pegang.
PCM16
  ↓
Base64
  ↓
JSON/WebSocket
  ↓
Base64 decode
  ↓
PCM16
Base64 hanya encoding transport.
Jangan sampai ada:
PCM16 → Base64 → dianggap format audio baru
Tidak. Audio tetap PCM16.
7. Parser JSON juga ada aturan arsitekturnya
Google WebSocket mengirim pesan server berupa JSON yang berisi serverContent, modelTurn, parts, inlineData, turnComplete, interrupted, dan sebagainya. �
Google AI for Developers
Jadi kita harus memisahkan:
WebSocket
   │
   ├── menerima bytes
   │
   └── Gemini parser
          │
          ├── audio chunk
          ├── interrupted
          ├── turnComplete
          └── error/status
WebSocket tidak boleh mengurus buffer audio playback.
Itu tugas Audio Engine.
Jadi untuk RepoVersiEnam, aturan idealnya
Saya akan jadikan kontrak seperti ini:
                    GEMINI LIVE
                         │
              PCM16 24kHz output
                         ↓
              ┌─────────────────┐
              │ WebSocket RX    │
              │ JSON parser     │
              └────────┬────────┘
                       │
                 audio chunk
                       ↓
              ┌─────────────────┐
              │ AUDIO ENGINE    │
              │                 │
              │ RX ring buffer  │
              │ playback queue  │
              │ flush()         │◄── interrupted
              └────────┬────────┘
                       │
                       ↓
                 AUDIO HAL
                       │
                PCM16 → PCM32
                       │
                       ↓
                    I2S
                       ↓
                   SPEAKER
Dan sisi mic:
MIC
 ↓
I2S 32-bit
 ↓
Audio HAL
 ↓
AEC / NSNet2
 ↓
PCM16
 ↓
Audio Engine
 ↓
20–40ms chunks
 ↓
WebSocket
 ↓
Base64 JSON
 ↓
Gemini
Kesimpulannya: iya, ada aturan yang cukup jelas untuk buffer dan streaming.

tambahan

BOOT / WAKE WORD
 ↓
WebSocket
 ↓
Gemini setupComplete
 ↓
KIRIM PEMICU UNTUK RESPONS AWAL
 ↓
Gemini menghasilkan audio
 ↓
Audio Engine
 ↓
Speaker
 ↓
"Halo, ada yang bisa dibantu?"
 ↓
baru MIC aktif untuk user


Target Repo6 setelah penyesuaian

MIC
 ↓
Audio HAL
 ↓
Audio Engine
 ↓
WebSocket
 ↓
Gemini
 ↓
WebSocket
 ↓
Gemini Audio Parser
 ↓
Base64 decode
 ↓
PCM Gemini
 ↓
Audio Engine Speaker Input
 ↓
Speaker
