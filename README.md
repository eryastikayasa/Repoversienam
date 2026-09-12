# RepoVersiEnam

Arsitektur baru firmware ESP32-S3, memakai RepoVersiEmpat sebagai referensi baseline.

## Baseline
- PlatformIO + ESP-IDF
- ESP32-S3 DevKitC-1
- Flash 16 MB
- CPU target 240 MHz
- Partition file di `partitions/partitions.csv`
- Struktur modular melalui `components/`

## Prinsip arsitektur
1. Pisahkan hardware abstraction, service, dan application orchestration.
2. Hindari task besar dengan stack berlebihan; setiap task punya ownership dan batas tanggung jawab jelas.
3. Komunikasi antar-modul memakai API/event/queue, bukan akses state global langsung.
4. WebSocket/network tidak boleh mengendalikan lifecycle audio/display secara langsung.
5. Build dan audit ELF dijalankan otomatis di GitHub Actions.

RepoVersiEmpat digunakan sebagai referensi implementasi yang sudah terbukti, bukan disalin mentah.
