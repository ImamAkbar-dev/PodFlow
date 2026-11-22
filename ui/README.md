# PodFlow UI (lapisan presentasi)

Antarmuka web untuk PodFlow. Folder ini **terpisah dari inti**: `src/`, `include/`, `tests/` dan protokol v1.1 tidak diubah.

```
Browser (UI + Web Audio) <-WebSocket-> Gateway (Python) <-TCP 8888-> Server C
```

Gateway hanya meneruskan frame 16-byte apa adanya. Batasan web dicatat di `../Kekurangan.md`.

## Struktur

| Folder | Isi |
|---|---|
| `gateway/` | Jembatan WebSocket <-> TCP, katalog, statistik |
| `web/` | HTML/JS tanpa build step; pemutaran lewat Web Audio |
| `tests/` | Tes khusus UI (paritas protokol, gateway) |
| `scripts/` | `run_demo.sh` untuk presentasi |

## Prinsip pengembangan

1. **Inti dibekukan.** Tidak ada perubahan di `src/`, `include/`, `tests/`, atau `docs/protocol_spec.md` (v1.1). Semua kode baru masuk ke `ui/`.
2. **Protokol tidak diubah.** Semua kebutuhan UI (daftar episode, kontrol, error) sudah tercakup pesan yang ada (`LIST`, `PLAY`, `PAUSE`, `SEEK`, `STOP`, `ECHO`, `ERROR`).
3. **Satu tab = satu koneksi TCP**, supaya semantik 1 sesi = 1 koneksi dan thread producer per sesi tetap berlaku.
4. **Backpressure dijaga.** Gateway berhenti membaca TCP saat buffer WebSocket penuh, sehingga tekanan dari klien lambat sampai ke server.
5. **Benchmark resmi tetap lewat klien C** dan `load_test.py`. Angka di UI hanya peraga.
6. **UI bersifat opsional.** Build dan tes inti tidak bergantung pada `ui/`.

## Rencana pengembangan

### Tahap 0 - Persiapan dan baseline
- [ ] Beri tag `v1.1-core` pada kondisi inti sekarang.
- [ ] Buat branch `ui`.
- [ ] Jalankan `make` dan tes inti sekali; simpan hasilnya sebagai baseline.
- [x] Kerangka folder `ui/` dan `Kekurangan.md`.

**Selesai bila:** tag dan branch ada, dan baseline tes inti hijau.

### Tahap 1 - Lapisan protokol di gateway
- [ ] Cocokkan tata letak header dengan `docs/protocol_spec.md` bagian 4 (urutan dan lebar field, big-endian).
- [ ] `gateway/protocol.py`: `pack_frame()` dan `recv_frame()` (validasi magic, versi, `payload_len <= 65536`).
- [ ] Tes unit untuk frame yang valid dan yang rusak.
- [ ] `tests/test_protocol_parity.py`: parse `include/protocol.h` dan bandingkan dengan konstanta di `protocol.py` dan `web/js/protocol.js`.

**Selesai bila:** gateway bisa HELLO -> WELCOME dan ECHO ke server nyata, dan tes paritas lolos.

### Tahap 2 - Sesi gateway
- [ ] `gateway/session.py`: satu koneksi TCP per WebSocket; teruskan frame dua arah apa adanya.
- [ ] Backpressure: berhenti membaca TCP saat buffer WebSocket tinggi, lanjutkan saat longgar.
- [ ] Aturan stale-DATA: abaikan `DATA` setelah PAUSE/SEEK/STOP sampai `ACK` diterima.
- [ ] Penanganan `ERROR`: teruskan ke browser; tutup sesi pada 100/101/103/503.
- [ ] Bersihkan sumber daya saat tab ditutup (kirim `STOP`, tutup soket).
- [ ] `tests/test_gateway.py`: PLAY lewat WebSocket, hitung total byte, cocokkan dengan `END.param` (sudah termasuk 44 byte header).

**Selesai bila:** satu sesi PLAY sampai END dengan total byte cocok, dan PAUSE/SEEK/STOP tidak menghasilkan DATA basi.

### Tahap 3 - Katalog dan server web
- [ ] `gateway/gateway.py`: layani `web/`, endpoint `/ws` dan `/api/catalog`.
- [ ] `gateway/catalog.py`: nama episode dari `LIST` (ACK berisi nama `*.wav` dipisah baris baru).
- [ ] Durasi sebagai pelengkap: dari header WAV (`DATA seq=0`) atau ukuran file; 1 chunk = 50 ms, 20 chunk per detik.

**Selesai bila:** `/api/catalog` mengembalikan daftar episode dari server.

### Tahap 4 - Pemutar minimal (suara keluar)
- [ ] `web/js/ws_client.js`: koneksi WebSocket, parse header 16 byte dan payload.
- [ ] `web/js/player.js`: ubah PCM16 stereo 44,1 kHz menjadi `AudioBuffer`, jadwalkan lewat `AudioContext.currentTime`.
- [ ] Jitter buffer: prebuffer 5-10 chunk (250-500 ms) sebelum mulai.
- [ ] `web/index.html` + `main.js`: daftar episode dan tombol Play. Tombol menjadi interaksi pertama (kebijakan autoplay).

**Selesai bila:** episode `sample_30s` terdengar sampai selesai tanpa putus.

### Tahap 5 - Kontrol penuh
- [ ] Pause dan Resume (`PLAY` payload kosong saat PAUSED).
- [ ] Seek lewat progress bar: `chunk = detik x 20 + 1`, dibatasi `1..total_chunks`; buang buffer lama lalu prebuffer ulang.
- [ ] Stop.
- [ ] Status UI mengikuti state machine IDLE/PLAYING/PAUSED; tombol yang tidak valid dinonaktifkan.
- [ ] Tampilkan kode error (termasuk 503 dengan coba ulang bertahap).

**Selesai bila:** semua skenario di spesifikasi (bagian 8.2-8.5) bisa dijalankan dari UI.

### Tahap 6 - Panel statistik (peraga)
- [ ] `gateway/stats.py`: inter-arrival jitter, sesi aktif, byte/s.
- [ ] `web/js/charts.js`: grafik level buffer, jitter, status sesi.
- [ ] Tombol ping berbasis `ECHO` untuk RTT.
- [ ] CPU/RSS server: gateway menyampel proses server, atau panel memakai hasil benchmark yang ada.
- [ ] Beri label "peraga, bukan data benchmark".

**Selesai bila:** grafik bergerak saat memutar dan saat beberapa tab dibuka.

### Tahap 7 - Demo dan dokumentasi
- [ ] `scripts/run_demo.sh`: nyalakan server C dan gateway sekaligus.
- [ ] Target `make ui` yang terpisah dari build inti (tanpa mengubah target yang ada).
- [ ] Bagian UI di laporan: diagram dua lapisan (inti vs presentasi) dan rujukan ke `Kekurangan.md`.
- [ ] Latihan demo sesuai skenario di bawah.

**Selesai bila:** demo 5 menit berjalan dua kali berturut-turut tanpa intervensi manual.

## Skenario demo (+-5 menit)

1. Nyalakan server dan buka halaman web; tampilkan daftar episode.
2. Putar episode; tunjukkan progress, status PLAYING, dan level buffer.
3. Pause, Resume, lalu Seek; buffer terbuang dan terisi lagi (epoch).
4. Beban banyak klien: beberapa tab atau `load_test.py` dengan N=10/50; panel statistik bergerak.
5. Klien lambat (`slow_client.py`): tunjukkan backpressure dan underrun, lalu pemulihan.
6. Bandingkan dengan Wireshark pada port 8888: header 16 byte sesuai spesifikasi.

## Risiko dan mitigasi

| Risiko | Mitigasi |
|---|---|
| Header di gateway menyimpang dari server | Tes paritas terhadap `protocol.h` dan uji nyata ke server |
| Buffer gateway menumpuk dan menyembunyikan backpressure | Hentikan pembacaan TCP saat buffer WebSocket penuh; uji dengan klien lambat |
| Underrun di browser karena jitter | Prebuffer 250-500 ms dan hening saat kosong |
| Pemutaran basi setelah Seek | Aturan stale-DATA di `session.py` dan `player.js` |
| Tab di latar belakang melambat | Andalkan jam audio, bukan `setTimeout`; catat sebagai batasan |
| Demo gagal di tempat | `run_demo.sh`, latihan dua kali, dan rekaman cadangan |
| UI mengganggu inti | Inti dibekukan; bandingkan hasil tes inti sebelum dan sesudah |

## Cara menjalankan (rencana)

```
pip install -r requirements.txt
make -C .. server          # atau jalankan server C seperti biasa
make run                   # gateway di http://localhost:8080
make test                  # tes UI
```

Server memakai API POSIX; jalankan di Linux atau WSL.

## Status

Kerangka sudah dibuat; logika belum diimplementasikan. Kemajuan dilacak lewat kotak centang di atas.
