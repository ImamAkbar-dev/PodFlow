# Kekurangan Antarmuka Web PodFlow

Dokumen ini mencatat bagian dari PodFlow (protokol v1.1 dan inti C) yang **tidak bisa, atau tidak sepenuhnya bisa, diwujudkan lewat antarmuka web**. Semua fitur protokol (HELLO, WELCOME, LIST, PLAY, PAUSE, SEEK, STOP, DATA, END, ACK, ECHO, ERROR) tetap berjalan, tetapi sebagian lewat gateway, dan sebagian aspek pengujian hanya valid memakai klien C.

Prinsip: inti (server C, protokol, klien C, tes, benchmark) tidak diubah. Web hanyalah lapisan presentasi.

---

## 1. Batasan arsitektur

| # | Kekurangan | Penyebab | Dampak | Penanganan |
|---|---|---|---|---|
| 1 | Browser tidak bisa membuka koneksi TCP mentah ke port 8888 | Browser hanya mengizinkan HTTP/WebSocket | Web tidak berbicara protokol PodFlow secara langsung, harus lewat gateway (WebSocket ke TCP) | Gateway meneruskan frame 16-byte apa adanya, protokol tidak diubah |
| 2 | Server tidak melihat browser, hanya gateway | Gateway memegang koneksi TCP ke server | Dari sisi server, "klien" adalah gateway. Perilaku end-to-end ke browser tidak diukur server | Satu tab = satu koneksi TCP, supaya semantik 1 sesi = 1 koneksi tetap |
| 3 | Gateway menambah latensi dan jitter | Ada satu hop dan satu proses tambahan | Angka jitter lewat web berbeda dari klien C | Benchmark resmi tetap memakai klien C dan `load_test.py` |
| 4 | Backpressure dan `SO_SNDTIMEO` (5 s) tidak teruji sampai ke browser | Timeout server berlaku pada koneksi gateway | Browser yang lambat hanya memengaruhi server jika gateway meneruskan tekanan (berhenti membaca TCP saat buffer WebSocket penuh) | Gateway dibuat menjaga backpressure; skenario klien lambat didemokan dengan `slow_client.py` |

## 2. Batasan pemutaran audio di browser

| # | Kekurangan | Penyebab | Dampak | Penanganan |
|---|---|---|---|---|
| 5 | Pacing 50 ms drift-free (`CLOCK_MONOTONIC`) tidak bisa direplikasi di browser | Timer JavaScript tidak presisi dan bisa dibatasi saat tab di latar belakang | Browser hanya menerima aliran yang sudah dipacing server dan menjadwalkannya lewat jam audio (`AudioContext.currentTime`) | Pacing tetap tanggung jawab server; browser memakai jitter buffer sendiri |
| 6 | Pemutaran otomatis tidak bisa | Kebijakan autoplay: `AudioContext` butuh interaksi pengguna | Pengguna harus menekan tombol dulu sebelum suara keluar | Tombol Play dianggap sebagai interaksi pertama |
| 7 | Skrip kontrol terjadwal (`--script "t:cmd,..."`) tidak tersedia di UI | Fitur itu ada di klien C untuk pengujian otomatis | Skenario waktu-ke-perintah otomatis tidak bisa dijalankan dari web | Tetap dijalankan lewat klien C |
| 8 | Sync skew antar klien tidak bisa diukur akurat di browser | Latensi perangkat keluaran audio dan resampling browser tidak terlihat | Grafik `sync_skew_vs_n` tidak bisa dihasilkan dari web | Gunakan hasil benchmark klien C |
| 9 | Resampling oleh browser | Perangkat audio bisa memakai sample rate selain 44,1 kHz | Audio mungkin di-resample otomatis, tidak berpengaruh pada data yang diterima | Tidak perlu penanganan; hanya dicatat |

## 3. Batasan pengujian dan pengukuran

| # | Kekurangan | Penyebab | Dampak | Penanganan |
|---|---|---|---|---|
| 10 | Verifikasi MD5 bit-exact (`test_md5.sh`) tidak berlaku untuk jalur web | Browser memutar audio, tidak menulis file hasil | Integritas byte hanya terbukti lewat klien C. Web hanya mencocokkan total byte di `END` | Pertahankan tes MD5 pada klien C |
| 11 | Jalur error fatal (`BAD_MAGIC` 100, `BAD_VERSION` 101, `PAYLOAD_TOO_LARGE` 103) tidak bisa dipicu dari UI | Gateway selalu membuat frame yang valid | Penanganan pelanggaran framing tidak bisa didemokan lewat web | Tetap diuji lewat `test_framing.sh` dan `test_protocol.c` |
| 12 | `INVALID_STATE` (300) dan `SEEK_OUT_OF_RANGE` (202) jarang atau tidak pernah muncul di UI | UI menonaktifkan tombol yang tidak valid dan membatasi rentang seek | Jalur error itu tidak terlihat saat demo web | Didemokan lewat klien C atau `test_control.sh` |
| 13 | Uji beban besar (N = 100 sampai 200 sesi) tidak realistis dari browser | Jumlah tab dan koneksi WebSocket browser terbatas, dan boros sumber daya | Web hanya menampilkan beberapa sesi nyata | Beban besar tetap memakai `load_test.py`; UI hanya menampilkan statistiknya |
| 14 | CPU dan RSS server tidak bisa dibaca langsung oleh browser | Server tidak menyediakan endpoint statistik, hanya menulis CSV | Panel statistik server tidak live kecuali gateway menyampelnya sendiri | Gateway membaca statistik proses server, atau panel memakai hasil benchmark yang sudah ada |
| 15 | Pengukuran timestamp kurang presisi | `performance.now()` lebih kasar dan ikut melewati hop gateway | Log CSV level nanodetik milik klien C tidak setara | Metrik UI diberi label "peraga", bukan data benchmark |

## 4. Batasan observasi jaringan

| # | Kekurangan | Penyebab | Dampak | Penanganan |
|---|---|---|---|---|
| 16 | Wireshark di sisi browser tidak menampilkan protokol PodFlow | Browser dan gateway berbicara WebSocket, bukan frame PodFlow | Header 16-byte hanya terlihat pada koneksi gateway ke server (port 8888) | Tangkap trafik di port 8888 untuk demo header |

## 5. Lingkungan

| # | Kekurangan | Penyebab | Dampak | Penanganan |
|---|---|---|---|---|
| 17 | Server tetap butuh Linux/WSL | Kode memakai API POSIX (`ppoll`, `pthread`) | Tidak bisa dijalankan native di Windows, meski browsernya di Windows | Jalankan server dan gateway di WSL atau Docker |

---

## Ringkasan untuk laporan

- Semua pesan protokol v1.1 tetap diimplementasikan (lewat gateway). Protokol tidak berubah dan tidak perlu naik versi.
- Yang tidak bisa dipaksakan ke web adalah **aspek pengujian dan pengukuran** (MD5, error framing fatal, uji beban besar, sync skew, benchmark jitter) serta **akses TCP langsung**.
- Bagian itu tetap dibuktikan memakai klien C dan skrip tes yang sudah ada.
