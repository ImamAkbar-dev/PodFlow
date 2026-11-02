# Concurrent Chunked Audio/Media Streaming Server (wav-streamer)

Proyek server media streaming berbasis C11 dan POSIX Sockets untuk mata kuliah Pemrograman Jaringan. Sistem ini mengalirkan audio WAV secara chunked menggunakan protokol biner kustom 16-byte di atas TCP (Port 8888).

## 🛠️ Spesifikasi Kunci
- **Bahasa**: C11 (`-std=c11 -Wall -Wextra -Werror -pthread -g`)
- **Port Default**: `8888`
- **Ukuran Chunk PCM Default**: `8820` byte (0.05 detik audio PCM 44.1kHz 16-bit Stereo)
- **Header Frame**: `16` byte (Network Byte Order / Big-Endian)

## 📁 Struktur Repositori
```text
wav-streamer/
├── Makefile                # Script kompilasi
├── README.md               # Dokumentasi proyek
├── .gitignore              # Abaikan file biner/media besar
├── include/                # Header file C (.h)
├── src/                    # Source code C (.c)
│   ├── common/             # Protocol & IO utils
│   ├── server/             # Code server
│   └── client/             # Code client
├── media/                  # Generator file audio WAV
├── tests/                  # Script pengujian otomatis
└── captures/               # Analisis Wireshark/tcpdump

Langkah 4: Buat File README.md
File ini adalah halaman petunjuk utama untuk kelompok kamu dan dosen/asisten praktikum.

Buat file baru bernama README.md di folder paling luar (wav-streamer/README.md), lalu isi dengan:

File: wav-streamer/README.md
Markdown
# Concurrent Chunked Audio/Media Streaming Server (wav-streamer)

Proyek server media streaming berbasis C11 dan POSIX Sockets untuk mata kuliah Pemrograman Jaringan. Sistem ini mengalirkan audio WAV secara chunked menggunakan protokol biner kustom 16-byte di atas TCP (Port 8888).

## 🛠️ Spesifikasi Kunci
- **Bahasa**: C11 (`-std=c11 -Wall -Wextra -Werror -pthread -g`)
- **Port Default**: `8888`
- **Ukuran Chunk PCM Default**: `8820` byte (0.05 detik audio PCM 44.1kHz 16-bit Stereo)
- **Header Frame**: `16` byte (Network Byte Order / Big-Endian)

## 📁 Struktur Repositori
wav-streamer/
├── Makefile                # Script kompilasi
├── README.md               # Dokumentasi proyek
├── .gitignore              # Abaikan file biner/media besar
├── include/                # Header file C (.h)
├── src/                    # Source code C (.c)
│   ├── common/             # Protocol & IO utils
│   ├── server/             # Code server
│   └── client/             # Code client
├── media/                  # Generator file audio WAV
├── tests/                  # Script pengujian otomatis
└── captures/               # Analisis Wireshark/tcpdump


## 👨‍💻 Pembagian Tugas Tim
- **Member 1**: Server Core (`server.c`, `io_utils.c`) & POSIX Multi-threading.
- **Member 2**: Protocol Engine (`protocol.c`, `pacing.c`, `ring_buffer.c`).
- **Member 3 (Bintang)**: Client (`client.c`), Script Media (`generate_wav.sh`), Automated Testing (`test_*.sh`), Git Repo, & Captures.

## 🚀 Cara Menjalankan
1. **Generate Media**: `./media/generate_wav.sh`
2. **Kompilasi**: `make clean && make`
3. **Jalankan Server**: `./server 8888`
4. **Jalankan Client**: `./client get 127.0.0.1 8888 sample_30s.wav output.wav`