# Tugas Sesi 4 - Antarmuka Web PodFlow

Dokumen ini membagi pekerjaan antarmuka web untuk tiga anggota dan menjelaskan alur pengerjaannya. Rencana teknis lengkap ada di `README.md`, dan batasan web ada di `../Kekurangan.md`.

| Anggota | Peran | Irisan |
|---|---|---|
| **Imam (Anggota 2)** | Pemilik protokol | Inti gateway: protokol dan sesi |
| **Rafly (Anggota 1)** | Pemilik server | Server web, statistik, operasional |
| **Bintang (Anggota 3)** | Pemilik klien dan pengujian | Pemutar, antarmuka, uji end-to-end, laporan |

---

## 1. Aturan dasar

1. **Inti dibekukan.** Tidak ada yang mengedit `src/`, `include/`, `tests/`, `docs/protocol_spec_v1.md` (v1.1), atau `Makefile` utama. Semua kode baru ada di `ui/`.
2. **Protokol tidak berubah.** Semua kebutuhan web sudah tercakup pesan yang ada (`LIST`, `PLAY`, `PAUSE`, `SEEK`, `STOP`, `ECHO`, `ERROR`).
3. **Server tidak diubah.** Statistik server dibaca dari `--stats-csv` yang sudah ada, bukan dari endpoint baru di `server.c`. Ini menggantikan butir "endpoint statistik di server" pada rencana awal (bagian 15.4 di `Pengerjaan 3 member.md`).
4. **Tiap orang hanya mengedit file miliknya.** Kebutuhan perubahan di file orang lain dikirim ke pemiliknya lewat komentar PR atau grup.
5. **Angka di UI hanya peraga.** Benchmark resmi tetap dari klien C dan `load_test.py`.

## 2. Kontrak antarmuka (dibekukan sebelum menulis kode)

Kontrak ini disepakati bertiga pada Tahap 0. Perubahan apa pun dibahas di grup, lalu `Tugas.md` ini diperbarui.

### 2.1 WebSocket `/ws`
- Satu pesan **biner** = satu frame PodFlow utuh (header 16 byte + payload), dua arah, tanpa pembungkus tambahan.
- Browser membangun frame sendiri lewat `protocol.js` dan mengirim `HELLO` lebih dulu (asumsi, konfirmasi di Tahap 0). Gateway tidak menyusun perintah atas nama browser.
- Satu koneksi WebSocket = satu koneksi TCP ke server C. Menutup tab berarti gateway mengirim `STOP` (bila perlu) lalu menutup soket.
- Pesan teks tidak dipakai.

### 2.2 Kode penutupan WebSocket (usulan)
| Kode | Arti |
|---|---|
| 4100 / 4101 / 4103 | Server menutup karena BAD_MAGIC / BAD_VERSION / PAYLOAD_TOO_LARGE |
| 4503 | SERVER_BUSY (batas `--max-clients`) |
| 4900 | Gateway tidak bisa terhubung ke server C |
| 4901 | Koneksi ke server C terputus atau timeout |

Error yang menjaga koneksi (102, 200, 201, 202, 300) tidak menutup WebSocket; frame `ERROR` diteruskan ke browser.

### 2.3 `GET /api/catalog`
```json
{"files":[{"name":"sample_30s.wav","size_bytes":5292044,"duration_s":30.0,"total_chunks":600}]}
```
`name` berasal dari `LIST`. Field lain boleh `null` bila tidak diketahui.

### 2.4 `GET /api/stats`
```json
{"t_s":12,"clients":3,"streaming":3,"chunks_sent":1800,"underruns":0,
 "late_resets":0,"max_lateness_us":412,"cpu_pct":4.2,"rss_kb":18432,"ws_sessions":3}
```
Tujuh kolom pertama sama dengan CSV statistik server. `cpu_pct`, `rss_kb` dan `ws_sessions` ditambahkan gateway.

### 2.5 Rumus bersama
- 1 chunk = 8820 byte = 50 ms, jadi 20 chunk per detik.
- Seek dari detik `s`: `chunk = floor(s x 20) + 1`, dibatasi `1..total_chunks`.
- `total_chunks` diambil dari katalog, atau dari ukuran data di header WAV (`DATA seq=0`): `ceil(data_size / 8820)`.
- Aturan stale-DATA: setelah mengirim `PAUSE`, `SEEK` atau `STOP`, abaikan `DATA` sampai `ACK` perintah itu diterima.

---

## 3. Pembagian tugas

### 3.1 Imam (Anggota 2): inti gateway
**Branch:** `feat/ui-m2-gateway-core`

| File | Tugas |
|---|---|
| `gateway/protocol.py` | `pack_frame()` dan `recv_frame()` (async). Cocokkan tata letak header dengan spesifikasi bagian 4 sebelum menulis. Validasi magic, versi, dan `payload_len <= 65536`. |
| `gateway/session.py` | Satu tab = satu koneksi TCP. Teruskan frame dua arah. Backpressure: berhenti membaca TCP saat buffer WebSocket tinggi. Aturan stale-DATA di sisi gateway. Teruskan `ERROR`, tutup WebSocket sesuai tabel 2.2. Bersihkan sumber daya saat tab ditutup. |
| `gateway/catalog.py` | Ambil daftar dari `LIST`. Durasi sebagai pelengkap (ukuran file di `media/` atau header WAV). |
| `tests/test_protocol_parity.py` | Parse `include/protocol.h`, bandingkan dengan konstanta di `protocol.py` dan `web/js/protocol.js`. |
| `tests/test_gateway.py` | PLAY lewat WebSocket ke server nyata, hitung total byte, cocokkan dengan `END.param` (sudah termasuk 44 byte header). Uji PAUSE/SEEK/STOP tanpa DATA basi. |
| `README.md`, `Tugas.md` | Pemelihara dokumen rencana. |

**Selesai bila:**
- [x] HELLO -> WELCOME, ECHO, LIST berhasil lewat gateway ke server nyata.
- [x] PLAY sampai END dengan total byte cocok.
- [x] PAUSE, SEEK, STOP tidak menghasilkan DATA basi.
- [x] Klien lambat tetap memicu backpressure ke server (diuji dengan `tests/slow_client.py`).
- [x] 503 menutup WebSocket dengan kode 4503.
- [x] `make -C ui test` hijau.

### 3.2 Rafly (Anggota 1): server web, statistik, operasional
**Branch:** `feat/ui-m1-gateway-stats`

| File | Tugas |
|---|---|
| `gateway/gateway.py` | Server aiohttp: layani `web/`, endpoint `/ws` (memakai `session.py` dari Imam), `/api/catalog`, `/api/stats`. Argumen: `--port`, `--server-host`, `--server-port`, `--stats-csv`, `--server-pid`. |
| `gateway/stats.py` | Baca baris terakhir `--stats-csv`, baca CPU dan RSS proses server dari `/proc/<pid>`, hitung `ws_sessions`. Hasilnya untuk `/api/stats`. |
| `web/js/charts.js` | Grafik level buffer, jitter peraga, sesi aktif, underrun/late_resets. Beri label "peraga, bukan data benchmark". |
| `Makefile` (di `ui/`) | Target `run`, `test`, `ui`. Makefile utama tidak diubah. |
| `scripts/run_demo.sh` | Nyalakan server C (dengan `--stats-csv`) dan gateway sekaligus, matikan keduanya saat selesai (`trap`). |

Tugas pengujian:
- [ ] Jalankan gate inti (`make`, `make test`, `bash tests/verify_podflow.sh`) **sebelum dan sesudah** pekerjaan UI, dan pastikan hasilnya sama.
- [ ] Uji `--max-clients 2` dengan tiga tab: tab ketiga mendapat 503.
- [ ] Uji beberapa tab bersamaan (3 sampai 10) tanpa mengganggu satu sama lain.

**Selesai bila:**
- [ ] `/api/stats` mengembalikan JSON sesuai kontrak 2.4 saat server berjalan.
- [ ] `scripts/run_demo.sh` menyalakan semuanya dengan satu perintah dan mematikannya bersih.
- [ ] Grafik bergerak saat ada sesi yang memutar.
- [ ] Hasil gate inti sebelum dan sesudah identik.

### 3.3 Bintang (Anggota 3): pemutar, antarmuka, laporan
**Branch:** `feat/ui-m3-player`

| File | Tugas |
|---|---|
| `web/index.html`, `css/style.css`, `js/main.js` | Tata letak, daftar episode, tombol Play/Pause/Resume/Stop, progress bar dan slider seek, status sesi, tampilan kode error. |
| `web/js/protocol.js` | Pack/unpack frame di browser (cocokkan dengan Imam). |
| `web/js/ws_client.js` | Koneksi WebSocket, kirim dan terima frame biner, parse header. |
| `web/js/player.js` | Web Audio: ubah PCM16 stereo 44,1 kHz menjadi `AudioBuffer`, jadwalkan lewat `AudioContext.currentTime`. Jitter buffer (prebuffer 5 sampai 10 chunk). Aturan stale-DATA. Status mengikuti IDLE/PLAYING/PAUSED. |
| Laporan | Bagian UI di `docs/report_loadtest_wireshark.md`, pembaruan `docs/demo_script.md`, perawatan `../Kekurangan.md`. |
| Bukti | Screenshot UI, rekaman cadangan demo, tangkapan Wireshark di port 8888. |

Tahap awal bisa dikerjakan tanpa menunggu gateway: buat gateway tiruan sederhana yang membalas sesuai kontrak 2.1 sampai 2.5.

**Selesai bila:**
- [ ] `sample_30s.wav` terdengar sampai selesai tanpa putus.
- [ ] Pause, Resume, Seek, Stop bekerja dan tombol yang tidak valid dinonaktifkan (menghindari error 300).
- [ ] Setelah Seek tidak ada suara dari DATA lama.
- [ ] Kode error dari server tampil jelas di UI.
- [ ] Screenshot dan bagian laporan UI selesai.

---

## 4. Alur pengerjaan

```
Tahap 0 -> Tahap 1 (paralel) -> Tahap 2 (integrasi) -> Tahap 3 (penyempurnaan) -> Tahap 4 (penutup)
```

### Tahap 0 - Persiapan (Imam, hari 1)
- [ ] Tanyakan ke dosen: (1) apakah gateway/proxy diperbolehkan, (2) apakah web boleh memutar audio lewat gateway, (3) ketentuan teknologi dan bobot nilai tambahan, (4) apakah web masuk demo dan laporan. Jika gateway tidak diizinkan, hentikan dan rencanakan ulang.
- [ ] Beri tag `v1.1-core` pada kondisi inti sekarang.
- [ ] Buat branch integrasi `ui` dari `develop`. Semua PR fitur menuju `ui`.
- [ ] Jalankan `make`, `make test`, `bash tests/verify_podflow.sh` sebagai baseline.
- [ ] Bahas dan setujui kontrak bagian 2 bersama (voice call).

### Tahap 1 - Kerja paralel (hari 2 sampai 4)
| Imam | Rafly | Bintang |
|---|---|---|
| `protocol.py`, lalu `session.py`, lalu tes | `gateway.py` (kerangka, `/api/stats`), `stats.py`, `Makefile`, `run_demo.sh` | `index.html`, `player.js`, `protocol.js`, `ws_client.js` dengan gateway tiruan |

Imam berada di jalur kritis, jadi `protocol.py` dan `session.py` di-merge ke `ui` lebih dulu supaya Rafly dan Bintang bisa mengujinya.

### Tahap 2 - Integrasi (hari 5)
Dipimpin Bintang lewat voice call:
1. `git checkout ui && git pull`
2. `make clean && make` pada inti (0 warning)
3. `bash media/generate_wav.sh`
4. Jalankan `ui/scripts/run_demo.sh`
5. Buka halaman web, putar `sample_30s.wav` sampai selesai
6. Uji Pause, Resume, Seek, Stop
7. `make -C ui test`

Bug integrasi dilaporkan ke pemilik file, bukan diperbaiki langsung oleh yang menemukan.

### Tahap 3 - Penyempurnaan (hari 6)
- Rafly dan Bintang: panel statistik, grafik, pengujian beberapa tab dan `--max-clients`.
- Imam: uji klien lambat dan skenario error (200, 202, 300, 503) lewat gateway.
- Bintang: skenario demo 5 menit sesuai `README.md`.

### Tahap 4 - Penutup (hari 7)
- [ ] Bandingkan gate inti dengan baseline Tahap 0 (harus identik).
- [ ] Merge `ui` ke `develop`, lalu `develop` ke `main`, beri tag `ui-done`.
- [ ] Latihan demo dua kali berturut-turut tanpa intervensi manual.
- [ ] Latihan tanya jawab lintas kode (bagian 6).

---

## 5. Aturan Git

1. Satu PR kecil per file atau per fitur, menuju branch `ui`.
2. Selalu `git pull origin ui` sebelum mulai dan sebelum membuka PR.
3. Merge PR berurutan, bukan bersamaan.
4. Syarat PR: `make -C ui test` hijau, inti tidak berubah (`git diff develop -- src include tests` kosong), ditinjau minimal satu rekan.
5. Commit memakai Conventional Commits, misalnya `feat(ui): ...`, `test(ui): ...`, `docs(ui): ...`.
6. Jangan commit file `.pcap`, file WAV, atau `__pycache__`.
7. Review silang: Bintang meninjau Imam (kecocokan dengan kontrak), Imam meninjau Rafly (penggunaan `session.py`), Rafly meninjau Bintang (pemakaian API stats dan pengujian).

## 6. Persiapan tanya jawab dosen

Dosen bisa bertanya ke siapa saja tentang bagian mana pun.

| Pertanyaan | Jawaban singkat |
|---|---|
| Mengapa butuh gateway? | Browser tidak bisa membuka TCP mentah; gateway meneruskan frame 16-byte tanpa mengubah protokol. |
| Apakah protokol berubah? | Tidak, tetap v1.1; semua fitur web memakai pesan yang sudah ada. |
| Bagaimana backpressure terjaga? | Gateway berhenti membaca TCP saat buffer WebSocket penuh, sehingga `SO_SNDTIMEO` server tetap berlaku. |
| Bagaimana mencegah suara basi setelah Seek? | Aturan stale-DATA di gateway dan di `player.js`: abaikan DATA sampai ACK diterima. |
| Bagaimana seek dari slider bekerja? | `chunk = floor(detik x 20) + 1`, dibatasi `1..total_chunks`. |
| Mengapa angka jitter di UI tidak dipakai di laporan? | Ada hop gateway dan timestamp browser kurang presisi; benchmark resmi dari klien C. |
| Dari mana statistik server? | Dari `--stats-csv` yang sudah ada dan `/proc`, tanpa mengubah server. |
| Apa yang tidak bisa dilakukan web? | Lihat `../Kekurangan.md` (MD5 bit-exact, error framing fatal, uji beban besar, sync skew). |

## 7. Risiko

| Risiko | Penanggung jawab | Mitigasi |
|---|---|---|
| Imam terlambat dan menahan yang lain | Imam | `protocol.py` di-merge lebih dulu; Bintang memakai gateway tiruan |
| Kontrak berubah di tengah jalan | Semua | Ubah hanya lewat grup, lalu perbarui dokumen ini |
| Inti tanpa sengaja ikut berubah | Rafly | Cek `git diff develop -- src include tests` di setiap PR dan gate sebelum/sesudah |
| Audio putus-putus | Bintang | Prebuffer 250 sampai 500 ms, hening saat buffer kosong |
| DATA basi setelah Seek | Imam, Bintang | Aturan stale-DATA di dua lapis, diuji khusus |
| Demo gagal di tempat | Rafly, Bintang | `run_demo.sh`, latihan dua kali, rekaman cadangan |
| Dosen tidak mengizinkan gateway | Imam | Ditanyakan di Tahap 0 sebelum menulis kode |

## 8. Kriteria selesai Sesi 4

- [ ] Semua checklist "Selesai bila" tiap anggota tercentang.
- [ ] Gate inti setelah UI identik dengan baseline Tahap 0.
- [ ] Demo 5 menit berjalan dua kali berturut-turut.
- [ ] `../Kekurangan.md` dan bagian UI di laporan selesai.
- [ ] Tag `ui-done` dibuat dan rekaman cadangan tersimpan di luar Git.
