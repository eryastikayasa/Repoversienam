# RepoVersiEnam

## Konsep Arsitektur Audio + WebSocket

RepoVersiEnam menggunakan arsitektur modular dengan tujuan utama: **mudah diperbaiki, mudah diuji, dan tidak membuat WebSocket menjadi pusat seluruh sistem audio**.

Referensi dasar arsitektur berasal dari RepoVersiEmpat, tetapi alur audio di RepoVersiEnam ditegaskan dengan pemisahan tanggung jawab yang lebih jelas.

## Alur Utama

```text
MIC
  │
  ▼
AUDIO HAL
  │  PCM audio
  ▼
AUDIO ENGINE
  │
  │  audio TX
  ▼
WEBSOCKET
  │
  │  transport
  ▼
GEMINI
  │
  │  audio/data RX
  ▼
WEBSOCKET
  │
  ▼
AUDIO ENGINE
  │
  │  PCM audio
  ▼
AUDIO HAL
  │
  ▼
SPEAKER
```

### Prinsip utama

> **Audio Engine adalah pusat pengelolaan audio. WebSocket hanya kurir. Audio HAL hanya menangani hardware audio.**

## Pembagian Tanggung Jawab

### 1. Audio HAL — Hardware Audio

Audio HAL hanya bertanggung jawab terhadap hardware audio, terutama:

- microphone
- speaker
- I2S
- konfigurasi sample rate
- pembacaan audio dari microphone
- pengiriman audio ke speaker
- konversi format yang memang diperlukan oleh hardware

Audio HAL **tidak mengetahui detail Gemini atau protokol WebSocket**.

```text
MIC ──► Audio HAL ──► Audio Engine
Audio Engine ──► Audio HAL ──► SPEAKER
```

### 2. Audio Engine — Pusat Audio

Audio Engine menjadi penghubung utama antara hardware audio dan jaringan.

Tanggung jawab:

- menerima audio dari Audio HAL
- mengelola buffer audio
- mengelola TX audio
- menerima audio RX dari WebSocket
- menyiapkan audio untuk speaker
- melakukan pemrosesan audio yang diperlukan
- mengatur aliran audio TX/RX

Audio Engine **tidak mengetahui detail koneksi TCP/TLS/WebSocket**.

```text
Audio HAL
    │
    ▼
Audio Engine
    │
    ├── TX ──► WebSocket
    │
    └── RX ◄── WebSocket
```

### 3. WebSocket — Transport / Kurir

WebSocket sengaja dibuat tipis.

Tanggung jawab WebSocket hanya:

- membuat koneksi
- memutus koneksi
- reconnect
- mengirim data
- menerima data
- menangani event koneksi
- menangani transport WebSocket
- menangani kebutuhan protokol Gemini yang memang khusus pada layer komunikasi

WebSocket **tidak boleh**:

- membaca microphone secara langsung
- menulis speaker secara langsung
- mengakses I2S audio secara langsung
- mengelola audio buffer milik Audio Engine
- melakukan audio processing
- membuat alur audio sendiri
- mengambil alih lifecycle Audio Engine

Dengan demikian jika koneksi Gemini atau format protokol berubah, perubahan utama cukup berada di layer WebSocket tanpa membongkar Audio HAL.

## Interface Antar-Modul

Hubungan modul dibuat satu arah dan sederhana:

```text
                 SEND AUDIO
Audio Engine ─────────────────► WebSocket

                 RECEIVE AUDIO
Audio Engine ◄───────────────── WebSocket
```

WebSocket tidak memanggil Audio HAL secara langsung.

Untuk data masuk, WebSocket meneruskan data ke Audio Engine melalui callback/event/queue yang disepakati.

```text
Gemini
  │
  ▼
WebSocket
  │
  │ RX event / callback / queue
  ▼
Audio Engine
  │
  ▼
Audio HAL
  │
  ▼
Speaker
```

## Struktur Komponen yang Direncanakan

```text
components/
│
├── audio_hal/
│   ├── include/
│   │   └── audio_hal.h
│   └── audio_hal.cpp
│
├── audio_engine/
│   ├── include/
│   │   └── audio_engine.h
│   ├── audio_engine.cpp
│   ├── audio_engine_mic.cpp
│   └── audio_engine_ingest.cpp
│
├── websocket/
│   ├── include/
│   │   └── websocket.h
│   ├── websocket.cpp
│   └── websocket_gemini.cpp
│
├── wifi_manager/
├── uart_control/
├── web_config/
└── display/
```

Struktur tersebut dapat berkembang jika implementasi membutuhkan pemisahan tambahan, tetapi batas tanggung jawab tetap dipertahankan.

## API WebSocket yang Sederhana

Interface awal WebSocket ditujukan sesederhana mungkin:

```cpp
void websocket_init();

bool websocket_connect();

bool websocket_send_audio(
    const uint8_t *data,
    size_t length
);

bool websocket_send_text(
    const char *text
);

void websocket_disconnect();

bool websocket_is_connected();
```

Untuk data masuk, WebSocket menyediakan mekanisme callback/event/queue menuju Audio Engine.

Contoh konsep:

```cpp
websocket_set_audio_callback(...);
websocket_set_event_callback(...);
```

Implementasi final API dapat disesuaikan setelah kontrak data Gemini ditetapkan.

## Aturan Dependency

Dependency utama harus mengikuti arah berikut:

```text
Audio HAL
    ▲
    │
Audio Engine
    ▲
    │
WebSocket
```

Secara konsep, WebSocket tidak boleh bergantung langsung kepada Audio HAL.

Lebih baik:

```text
WebSocket ──► Audio Engine
```

daripada:

```text
WebSocket ──► Audio HAL
```

Hal ini membuat WebSocket dapat diganti, diperbaiki, atau diuji tanpa mengubah driver audio.

## TX dan RX Dipisahkan

### TX — Microphone ke Gemini

```text
MIC
 ↓
Audio HAL
 ↓
Audio Engine
 ↓
TX Queue / Buffer
 ↓
WebSocket
 ↓
Gemini
```

### RX — Gemini ke Speaker

```text
Gemini
 ↓
WebSocket
 ↓
RX Queue / Buffer
 ↓
Audio Engine
 ↓
Audio HAL
 ↓
Speaker
```

TX dan RX tidak boleh dicampur menjadi satu fungsi besar.

## Queue dan Buffer

Queue/buffer menjadi tanggung jawab Audio Engine untuk aliran audio.

Konsep:

```text
                 ┌───────────────┐
MIC ─► Audio HAL │               │
                 │  Audio Engine │ ──► TX Queue ──► WebSocket
                 │               │
Speaker ◄ Audio HAL ◄ RX Queue ◄─│ ◄────────────── WebSocket
                 └───────────────┘
```

WebSocket tidak memiliki hak kepemilikan atas buffer audio utama.

## Error Handling

Error dipisahkan berdasarkan layer.

### Audio HAL

Contoh:

```text
I2S error
MIC error
SPEAKER error
DMA error
```

### Audio Engine

Contoh:

```text
TX buffer penuh
RX buffer penuh
Audio underrun
Audio overflow
```

### WebSocket

Contoh:

```text
DNS error
TLS error
Connection failed
Connection closed
Send failed
Receive failed
Protocol error
```

Error WebSocket tidak boleh langsung mengendalikan hardware speaker atau microphone.

## Reconnect

Reconnect menjadi tanggung jawab WebSocket.

```text
Connection Lost
      │
      ▼
WebSocket
      │
      ├── cleanup transport
      ├── delay/backoff
      └── reconnect
              │
              ▼
         Connection OK
```

Audio Engine cukup menerima status koneksi melalui event.

## State Koneksi

State WebSocket yang direncanakan:

```text
DISCONNECTED
      │
      ▼
CONNECTING
      │
      ▼
CONNECTED
      │
      ├──────────────┐
      ▼              │
ERROR / CLOSED ──────┘
```

State koneksi tidak menjadi state audio.

## Gemini Protocol

Detail format Gemini ditempatkan di layer WebSocket/Gemini adapter.

Tujuannya agar Audio Engine hanya melihat konsep sederhana:

```text
send audio
receive audio
connection status
```

Audio Engine tidak perlu mengetahui:

- JSON Gemini
- header WebSocket
- URI Gemini
- TLS detail
- authentication transport
- format event jaringan

## Prinsip Debugging

Arsitektur dibuat supaya sumber masalah dapat dipersempit.

### Jika microphone bermasalah

```text
Audio HAL
```

### Jika microphone normal tetapi TX tidak berjalan

```text
Audio Engine
```

### Jika TX keluar dari Audio Engine tetapi Gemini tidak menerima

```text
WebSocket
```

### Jika Gemini mengirim audio tetapi tidak sampai speaker

Periksa berurutan:

```text
WebSocket
   ↓
Audio Engine
   ↓
Audio HAL
   ↓
Speaker
```

## Prinsip Stack dan Task

Setiap task harus memiliki tanggung jawab sempit.

Hindari satu task besar seperti:

```text
WebSocket Task
 ├── WebSocket
 ├── JSON
 ├── Audio
 ├── I2S
 ├── Speaker
 ├── Microphone
 └── Gemini
```

Targetnya adalah:

```text
WebSocket Task
 └── transport/network

Audio Engine Task
 └── audio pipeline

Audio HAL
 └── hardware
```

Dengan pembagian ini, masalah stack overflow atau deadlock lebih mudah dilokalisasi.

## Prinsip Utama RepoVersiEnam

1. **WebSocket hanya kurir.**
2. **Audio Engine adalah pusat pipeline audio.**
3. **Audio HAL hanya menangani hardware audio.**
4. **WebSocket tidak mengakses I2S secara langsung.**
5. **WebSocket tidak menulis speaker secara langsung.**
6. **WebSocket tidak membaca microphone secara langsung.**
7. **TX dan RX audio dipisahkan.**
8. **Buffer audio dimiliki Audio Engine.**
9. **Status koneksi dikirim sebagai event/status, bukan kontrol hardware langsung.**
10. **Perubahan Gemini sebisa mungkin hanya menyentuh layer WebSocket/Gemini adapter.**
11. **Setiap task mempunyai tanggung jawab yang sempit.**
12. **API antar-komponen harus sederhana dan stabil.**

## Target Akhir

Target arsitektur RepoVersiEnam adalah:

```text
┌──────────┐
│   MIC    │
└────┬─────┘
     ▼
┌──────────┐
│ AUDIO HAL│
└────┬─────┘
     ▼
┌──────────────┐
│ AUDIO ENGINE │
└────┬─────────┘
     │
     ▼
┌──────────────┐
│  WEBSOCKET   │  ← hanya kurir
└────┬─────────┘
     ▼
┌──────────────┐
│    GEMINI    │
└────┬─────────┘
     │
     ▼
┌──────────────┐
│  WEBSOCKET   │  ← hanya kurir
└────┬─────────┘
     ▼
┌──────────────┐
│ AUDIO ENGINE │
└────┬─────────┘
     ▼
┌──────────┐
│ AUDIO HAL│
└────┬─────┘
     ▼
┌──────────┐
│ SPEAKER  │
└──────────┘
```

**Konsep ini menjadi kontrak arsitektur sebelum implementasi WebSocket RepoVersiEnam dimulai.**
