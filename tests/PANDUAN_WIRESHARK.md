# Panduan Skenario Wireshark - PodFlow

Panduan ini menjelaskan cara merekam dan menganalisis trafik PodFlow dengan `tcpdump` dan Wireshark, skenario demi skenario, supaya bukti untuk laporan (bagian "Wireshark dan uji beban", 20% nilai) lengkap dan bisa diulang oleh siapa pun di tim.

Rujukan: `docs/protocol_spec.md` (bagian 4, 5, 8, 9), `docs/report_loadtest_wireshark.md` (bagian 5). (Catatan: `captures/README_captures.md` saat ini masih kosong; panduan ini bisa menjadi isinya.)

---

## 1. Apa yang perlu dibuktikan

| Bukti | Skenario | Bagian laporan |
|---|---|---|
| Three-way handshake TCP | S1 | 5.1 |
| `HELLO` / `WELCOME` | S1 | 5.1 |
| `PLAY` / `ACK`, `DATA seq=0` (header WAV 44 byte) | S1 | 5.1 |
| Header `DATA seq=1` (`payload_len=8820`) di hex dump | S1 | 5.2 |
| Ritme 50 ms dan throughput ~176,7 KB/s per klien | S1, S7 | 5.3 |
| `END` dengan `param` = total byte stream | S1 | 5.3 |
| Terminasi koneksi (`FIN`) | S1 | 5.1 |
| Jeda saat `PAUSE`, dan lanjut saat `RESUME` | S2 | tambahan |
| `SEEK`: ACK lalu `DATA` dari chunk target, header WAV tidak dikirim ulang | S3 | tambahan |
| `STOP`: ACK tanpa frame `END` | S4 | tambahan |
| `ERROR` 200 / 202 / 300, koneksi tetap terbuka | S5 | tambahan |
| `ERROR 503 SERVER_BUSY` lalu koneksi ditutup | S6 | tambahan |
| Banyak klien serentak | S7 | 5.3 |
| Back-pressure: `zero window` lalu klien macet di-drop sekitar 5 detik | S8 | 4 |
| Dua sisi gateway (WebSocket dan TCP 8888) | S9 | bagian UI |

Semua angka dan screenshot harus berasal dari rekaman asli. Jika hasilnya kurang bagus, laporkan apa adanya beserta analisis penyebabnya.

---

## 2. Persiapan

### 2.1 Alat
- **Di WSL2 / Linux:** `tcpdump` (`sudo apt install tcpdump`), opsional `tshark` (`sudo apt install tshark`), `iproute2` (`ss`).
- **Di Windows:** Wireshark (untuk membuka dan menganalisis berkas `.pcap`).

### 2.2 Aturan penting untuk WSL2
Trafik loopback `127.0.0.1` di WSL2 **tidak** melewati Npcap Windows, jadi Wireshark Windows tidak bisa merekamnya langsung. Cara yang benar:

1. **Rekam di dalam WSL2** dengan `tcpdump -i lo`.
2. **Buka berkas `.pcap` di Wireshark Windows** untuk analisis.

Akses berkas dari Windows lewat `\\wsl$\<nama-distro>\home\<user>\podflow\captures\` (atau `\\wsl.localhost\...`), atau salin dengan `cp captures/x.pcap /mnt/c/Users/<user>/Desktop/`.

### 2.3 Persiapan proyek
Jalankan dari akar repo:

```bash
make clean && make                 # 0 warning
bash media/generate_wav.sh         # membuat sample_30s.wav, sample_5m.wav, sample_tail.wav
mkdir -p captures
ulimit -n 65535                    # untuk skenario banyak klien
```

### 2.4 Template perintah rekam
```bash
sudo tcpdump -i lo port 8888 -s 0 -Z "$USER" -w captures/<nama>.pcap
```

- `-s 0`: simpan paket utuh.
- `-Z "$USER"`: tcpdump menulis berkas sebagai pengguna Anda. Tanpa ini, di Ubuntu tcpdump bisa gagal menulis ke folder repo (`Permission denied`) karena melepas hak akses ke pengguna `tcpdump`.
- Hentikan dengan `Ctrl+C`, **setelah** klien selesai dan koneksi tertutup (supaya paket `FIN` ikut terekam).

### 2.5 Aturan umum merekam
- Mulai rekaman **sebelum** klien dijalankan, supaya handshake TCP tertangkap.
- Satu skenario = satu berkas `.pcap`, supaya mudah dianalisis dan dijelaskan.
- Beri nama dengan pola `captures/s<nomor>_<singkatan>.pcap`.
- Berkas `.pcap` **tidak** di-commit (sudah di `.gitignore`). Simpan salinan cadangan di luar Git (Drive).

---

## 3. Skenario

Setiap skenario memakai tiga terminal: **A** (server), **B** (rekaman), **C** (klien). Urutan: B dimulai, lalu C dijalankan, lalu B dihentikan.

### S1 - Satu klien, stream penuh (sample_30s)
**Tujuan:** handshake, frame kontrol, header `DATA`, ritme 50 ms, `END`, `FIN`.

```bash
# A
./server 8888 --stats-csv /tmp/stats.csv
# B
sudo tcpdump -i lo port 8888 -s 0 -Z "$USER" -w captures/s1_stream.pcap
# C
./client play 127.0.0.1 8888 sample_30s.wav /tmp/out_s1.wav --csv /tmp/client_s1.csv
```

**Yang dicari di Wireshark:**
- `tcp.flags.syn == 1`: tiga paket pertama (SYN, SYN-ACK, ACK).
- `HELLO` (type `01`) lalu `WELCOME` (type `02`, `param = 8820`, payload `podflow/1`).
- `PLAY` (type `04`, payload `sample_30s.wav`), lalu `ACK` (type `0a`).
- `DATA seq=0` dengan `payload_len = 44` (header WAV), lalu `DATA seq=1` dengan `payload_len = 8820`.
- Jeda antar `DATA` sekitar 0,050 s.
- `END` dengan `seq = 601` dan `param = 5292044`.
- `tcp.flags.fin == 1`: terminasi koneksi.

**Hasil yang diharapkan** (dari spesifikasi bagian 8.2): 601 frame `DATA` (seq 0 sampai 600), 1 frame `END`. Jumlah frame `DATA` yang terhitung lewat filter byte bisa sedikit lebih rendah jika ada beberapa frame digabung dalam satu segmen TCP; gunakan dissector (bagian 7) untuk hitungan yang tepat.

**Screenshot:** handshake, `HELLO`, `DATA seq=1` (hex dump), I/O Graph, `END`, `FIN`.

### S2 - PAUSE dan RESUME
**Tujuan:** menunjukkan aliran berhenti saat `PAUSE` dan lanjut saat `RESUME`.

```bash
# A
./server 8888
# B
sudo tcpdump -i lo port 8888 -s 0 -Z "$USER" -w captures/s2_pause.pcap
# C  (3 detik setelah DATA pertama: pause, 5 detik: lanjut)
./client play 127.0.0.1 8888 sample_30s.wav /tmp/out_s2.wav --script "3:pause,5:play"
```

**Yang dicari:**
- `PAUSE` (type `05`, `payload_len = 0`) dari klien, lalu `ACK` dengan `param` = nomor chunk berikutnya (sekitar 60).
- **Jeda sekitar 2 detik** tanpa frame `DATA`. Gunakan kolom `frame.time_delta_displayed` pada filter `DATA`.
- `PLAY` dengan `payload_len = 0` (resume), `ACK` dengan `param` sama dengan chunk berikutnya, lalu `DATA` dilanjutkan tanpa chunk hilang atau ganda.

**Screenshot:** daftar paket yang memperlihatkan jeda (kolom delta), dan `ACK` setelah `PAUSE`.

### S3 - SEEK
**Tujuan:** `SEEK` membuang buffer, `ACK` target, `DATA` dilanjutkan dari chunk target tanpa header WAV.

```bash
# C  (2 detik setelah DATA pertama: loncat ke chunk 300)
./client play 127.0.0.1 8888 sample_30s.wav /tmp/out_s3.wav --script "2:seek=300"
```
(Rekam ke `captures/s3_seek.pcap`.)

**Yang dicari:**
- `SEEK` (type `06`, `param = 300`).
- Mungkin ada beberapa `DATA` yang masih "dalam perjalanan" sebelum `ACK` (data basi). Klien harus mengabaikannya.
- `ACK` dengan `param = 300`, lalu `DATA seq=300`. **Tidak ada `DATA seq=0` baru.**

**Screenshot:** `SEEK`, `ACK`, dan `DATA seq=300` berurutan.

### S4 - STOP
**Tujuan:** `STOP` menghasilkan `ACK` dan **tidak** ada `END`.

```bash
./client play 127.0.0.1 8888 sample_30s.wav /tmp/out_s4.wav --script "3:stop"
```
(Rekam ke `captures/s4_stop.pcap`.)

**Yang dicari:** `STOP` (type `07`), `ACK` dengan `param` = seq terakhir terkirim, lalu `DATA` berhenti. Filter `tcp.payload[0:4] == 57:53:01:09` (type `END`) tidak boleh menghasilkan paket apa pun.

### S5 - Error yang menjaga koneksi (200, 202, 300)
**Tujuan:** server membalas `ERROR` dan koneksi tetap terbuka.

```bash
# File tidak ada dan path traversal -> ERROR 200
./client play 127.0.0.1 8888 tidak_ada.wav /tmp/x.wav
./client play 127.0.0.1 8888 ../x /tmp/x.wav
```
Untuk `202` (SEEK di luar rentang) dan `300` (perintah di state salah), jalankan `bash tests/test_control.sh` sambil merekam. Skrip itu memulai server sendiri; lihat variabel `TEST_PORT` di awal skrip (saat ini `18890`) dan rekam port itu:

```bash
sudo tcpdump -i lo port 18890 -s 0 -Z "$USER" -w captures/s5_errors.pcap
```

**Yang dicari:** `ERROR` (type `ff`) dengan `param` = `200` (`0x000000c8`), `202`, atau `300` (`0x0000012c`), payload berupa teks UTF-8 tanpa NUL, dan **tidak ada `FIN` dari server** setelahnya (koneksi tetap terbuka).

### S6 - SERVER_BUSY (503)
**Tujuan:** batas `--max-clients` menolak klien ke-3 dan menutup koneksi.

```bash
# A
./server 8888 --max-clients 2
# B
sudo tcpdump -i lo port 8888 -s 0 -Z "$USER" -w captures/s6_busy.pcap
# C  (dua klien panjang di latar belakang, lalu klien ketiga)
./client play 127.0.0.1 8888 sample_5m.wav /tmp/c1.wav &
./client play 127.0.0.1 8888 sample_5m.wav /tmp/c2.wav &
sleep 1
./client play 127.0.0.1 8888 sample_30s.wav /tmp/c3.wav
```

**Yang dicari:** pada koneksi ketiga, server langsung mengirim `ERROR` dengan `seq = 0` dan `param = 503` (`0x000001f7`), payload `SERVER_BUSY`, lalu **`FIN` dari server** (ditutup segera).

Setelah selesai, hentikan dua klien latar belakang (`kill %1 %2`) atau tunggu selesai.

### S7 - Banyak klien serentak
**Tujuan:** throughput agregat dan perilaku saat banyak klien.

```bash
# B  (rekam header saja agar berkas tidak raksasa)
sudo tcpdump -i lo port 8888 -s 96 -Z "$USER" -w captures/s7_load50.pcap
# C  (skrip ini menyalakan server di port 8888 sendiri; jangan jalankan server lain)
bash m1-verify/load.sh 50
```

- **Mengapa `-s 96`:** 50 klien x 30 detik x ~177 KB/s menghasilkan sekitar 265 MB jika dipotong penuh. Dengan `-s 96` hanya header (Ethernet + IP + TCP + 16 byte header PodFlow) yang disimpan. Isi payload audio tidak bisa dilihat, tapi header dan statistik tetap valid. Wireshark mungkin menampilkan peringatan "truncated"; itu normal.
- Jika ingin analisis isi paket, ulangi dengan `-s 0` pada N yang kecil (misalnya 5).

**Yang dicari:** `Statistics > Conversations > TCP` menampilkan 50 percakapan; I/O Graph agregat mendekati N x 176,7 KB/s (50 klien, sekitar 8,8 MB/s). Pada laporan, bandingkan dengan `results/n_50/` (jitter, CPU, RSS).

### S8 - Klien macet (back-pressure)
**Tujuan:** server mendeteksi klien yang tidak membaca dan memutusnya sekitar 5 detik (`SO_SNDTIMEO`) tanpa mengganggu klien lain.

```bash
# A
./server 8888
# B
sudo tcpdump -i lo port 8888 -s 0 -Z "$USER" -w captures/s8_backpressure.pcap
# C1  (klien macet: PLAY sample_5m, lalu tidur 60 detik tanpa membaca)
python3 tests/slow_client.py
# C2  (klien normal sebagai pembanding)
./client play 127.0.0.1 8888 sample_30s.wav /tmp/out_s8.wav
# D  (opsional: lihat antrean kirim)
watch -n 1 "ss -tin 'sport = :8888'"
```

**Yang dicari:**
- Filter `tcp.analysis.zero_window` dan `tcp.analysis.window_full`: jendela terima klien macet penuh. Saat buffer soket sudah penuh, server berhenti bisa mengirim.
- Pada `ss -tin`, kolom `Send-Q` milik koneksi macet naik.
- Sekitar 5 detik kemudian server memutus koneksi (`FIN` atau `RST`), sementara stream klien C2 tetap lancar sampai `END`.

Catatan: buffer soket di loopback cukup besar, jadi diperlukan beberapa puluh detik sampai jendela benar-benar penuh. Waktu "sekitar 5 detik" dihitung sejak server tidak bisa mengirim lagi, bukan sejak klien mulai tidur. Catat waktu sebenarnya dari rekaman.

### S9 - Antarmuka web (dua sisi gateway)
**Berlaku setelah gateway selesai dibuat** (rencana di `ui/README.md`).

```bash
sudo tcpdump -i lo "port 8888 or port 8080" -s 0 -Z "$USER" -w captures/s9_web.pcap
```
Putar satu episode dari browser, lalu analisis:

- `tcp.port == 8080`: HTTP `Upgrade: websocket` dan balasan `101 Switching Protocols`, lalu frame WebSocket biner (filter `websocket`).
- `tcp.port == 8888`: protokol PodFlow murni antara gateway dan server C.
- Cocokkan `seq` dan `payload_len` frame PodFlow di kedua sisi; gateway hanya meneruskan, tidak mengubah.

Wireshark di sisi browser tidak menampilkan header 16 byte secara langsung kecuali payload WebSocket diperiksa; lihat `Kekurangan.md` poin 16.

---

## 4. Filter tampilan (display filter)

Header PodFlow: byte 0-1 magic `57 53`, byte 2 versi `01`, byte 3 tipe pesan, byte 4-7 `seq`, byte 8-11 `payload_len`, byte 12-15 `param`.

| Tujuan | Filter |
|---|---|
| Semua trafik PodFlow | `tcp.port == 8888` |
| Hanya segmen yang diawali frame PodFlow | `tcp.port == 8888 && tcp.payload[0:2] == 57:53` |
| Arah server ke klien | `tcp.srcport == 8888` |
| Handshake TCP | `tcp.flags.syn == 1` |
| Terminasi | `tcp.flags.fin == 1 \|\| tcp.flags.reset == 1` |
| `HELLO` | `tcp.payload[0:4] == 57:53:01:01` |
| `WELCOME` | `tcp.payload[0:4] == 57:53:01:02` |
| `LIST` | `tcp.payload[0:4] == 57:53:01:03` |
| `PLAY` | `tcp.payload[0:4] == 57:53:01:04` |
| `PAUSE` | `tcp.payload[0:4] == 57:53:01:05` |
| `SEEK` | `tcp.payload[0:4] == 57:53:01:06` |
| `STOP` | `tcp.payload[0:4] == 57:53:01:07` |
| `DATA` | `tcp.payload[0:4] == 57:53:01:08` |
| `END` | `tcp.payload[0:4] == 57:53:01:09` |
| `ACK` | `tcp.payload[0:4] == 57:53:01:0a` |
| `ECHO` | `tcp.payload[0:4] == 57:53:01:0b` |
| `ERROR` | `tcp.payload[0:4] == 57:53:01:ff` |
| `ERROR` 503 | `tcp.payload[0:4] == 57:53:01:ff && tcp.payload[12:4] == 00:00:01:f7` |
| `ERROR` 200 | `tcp.payload[0:4] == 57:53:01:ff && tcp.payload[12:4] == 00:00:00:c8` |
| `ERROR` 300 | `tcp.payload[0:4] == 57:53:01:ff && tcp.payload[12:4] == 00:00:01:2c` |
| `DATA` dengan `seq = 0` (header WAV) | `tcp.payload[0:4] == 57:53:01:08 && tcp.payload[4:4] == 00:00:00:00` |
| `DATA` dengan `seq = 1` | `tcp.payload[0:4] == 57:53:01:08 && tcp.payload[4:4] == 00:00:00:01` |
| `DATA` berpayload 8820 byte | `tcp.payload[0:4] == 57:53:01:08 && tcp.payload[8:4] == 00:00:22:74` |
| `END` sample_30s (`param = 5292044`) | `tcp.payload[0:4] == 57:53:01:09 && tcp.payload[12:4] == 00:50:c0:0c` |
| Retransmisi | `tcp.analysis.retransmission` |
| Jendela penuh / nol | `tcp.analysis.window_full \|\| tcp.analysis.zero_window` |

Catatan:
- `tcp.payload[...]` hanya cocok jika frame PodFlow **dimulai** di awal segmen TCP. Di loopback dengan MTU besar hal itu hampir selalu benar, tapi jika beberapa frame digabung dalam satu segmen, hanya yang pertama yang cocok. Untuk hasil yang tepat, pakai dissector di bagian 7 dan filter seperti `podflow.msg_type == 8`.
- Tanda `\|\|` pada tabel di atas ditulis dengan garis miring terbalik hanya agar tabel Markdown tidak rusak; ketik `||` biasa di Wireshark.

Nilai heksadesimal yang dipakai: `8820 = 0x00002274`, `44 = 0x0000002c`, `5292044 = 0x0050c00c`, `503 = 0x000001f7`, `200 = 0x000000c8`, `300 = 0x0000012c`.

---

## 5. Fitur Wireshark yang berguna

| Fitur | Menu | Kegunaan |
|---|---|---|
| Hex dump header | klik paket, lihat panel bawah (Bytes) | Bukti tata letak header 16 byte big-endian |
| Kolom selisih waktu | klik kanan `Time delta from previous displayed frame` lalu *Apply as Column* (`frame.time_delta_displayed`) | Ritme 50 ms; terapkan bersama filter `DATA` |
| Follow TCP Stream | `Analyze > Follow > TCP Stream` | Melihat satu sesi utuh |
| I/O Graph | `Statistics > I/O Graphs` | Throughput per detik |
| Conversations | `Statistics > Conversations > TCP` | Jumlah koneksi dan byte per koneksi |
| Flow Graph | `Statistics > Flow Graph` (TCP flows) | Diagram urutan untuk laporan |
| Expert Info | `Analyze > Expert Information` | Retransmisi, zero window, dan sejenisnya |

### Pengaturan I/O Graph untuk S1
- Interval: `1 sec`
- Display filter: `tcp.srcport == 8888 && tcp.payload[0:4] == 57:53:01:08`
- Sumbu Y: `Bytes`
- Yang diharapkan: garis datar sekitar **176,7 KB/s** per klien, yaitu (16 + 8820) byte setiap 0,050 s = 176.720 B/s. Jika angkanya berbeda, laporkan angka yang terukur.

---

## 6. Analisis cepat dengan tshark

`tshark` (bagian dari Wireshark; di WSL2: `sudo apt install tshark`) bisa mengulang analisis lewat baris perintah. Contoh di bawah perlu disesuaikan jika nama berkas berbeda.

```bash
# Jumlah frame DATA dari server
tshark -r captures/s1_stream.pcap -Y "tcp.srcport==8888 && tcp.payload[0:4]==57:53:01:08" | wc -l

# Statistik jeda antar DATA (opsi -2 wajib untuk kolom delta berbasis filter)
tshark -2 -r captures/s1_stream.pcap \
  -Y "tcp.srcport==8888 && tcp.payload[0:4]==57:53:01:08" \
  -T fields -e frame.time_delta_displayed \
| awk 'NR>2 { if (min==""||$1<min) min=$1; if ($1>max) max=$1; s+=$1; n++ }
       END { printf "n=%d  mean=%.2f ms  min=%.2f ms  max=%.2f ms\n", n, s/n*1000, min*1000, max*1000 }'

# Byte per detik dari server
tshark -r captures/s1_stream.pcap -q -z io,stat,1,"tcp.srcport==8888"

# Daftar percakapan TCP
tshark -r captures/s7_load50.pcap -q -z conv,tcp

# Semua frame ERROR dengan waktu relatif
tshark -r captures/s6_busy.pcap -Y "tcp.payload[0:4]==57:53:01:ff" -T fields -e frame.time_relative -e tcp.payload
```

Dua `DATA` pertama (seq 0 dan 1) dikirim bersamaan, jadi selisihnya mendekati nol dan dilewati oleh `NR>2`. Nilai `min` dan `max` yang kecil dan besar pada klien yang sibuk adalah data nyata yang perlu dilaporkan, bukan kesalahan.

---

## 7. Dissector Lua (opsional)

Tanpa dissector, Wireshark hanya menampilkan payload sebagai "Data". Dengan dissector ini Wireshark mengenali PodFlow: kolom *Info* menampilkan tipe pesan, `seq`, dan `param`, dan Anda bisa memakai filter seperti `podflow.msg_type == 8` atau `podflow.seq == 1`.

**Pemasangan:** simpan sebagai `podflow.lua` di `%APPDATA%\Wireshark\plugins\` (buat folder `plugins` bila belum ada), lalu restart Wireshark atau tekan `Ctrl+Shift+L` (*Reload Lua Plugins*).

> Catatan: skrip ini belum diuji di Wireshark dari sisi saya. Jika ada galat, cek `Help > About Wireshark > Plugins` dan pesan Lua di `Analyze > ...` lalu perbaiki.

```lua
local pf = Proto("podflow", "PodFlow Protocol")
local F = pf.fields
local types = {[1]="HELLO", [2]="WELCOME", [3]="LIST", [4]="PLAY", [5]="PAUSE",
               [6]="SEEK", [7]="STOP", [8]="DATA", [9]="END", [10]="ACK",
               [11]="ECHO", [255]="ERROR"}

F.magic    = ProtoField.uint16("podflow.magic", "Magic", base.HEX)
F.version  = ProtoField.uint8("podflow.version", "Version")
F.msg_type = ProtoField.uint8("podflow.msg_type", "Message type", base.DEC, types)
F.seq      = ProtoField.uint32("podflow.seq", "Sequence")
F.len      = ProtoField.uint32("podflow.payload_len", "Payload length")
F.param    = ProtoField.uint32("podflow.param", "Param")
F.payload  = ProtoField.bytes("podflow.payload", "Payload")

local function pdu_len(tvb, pinfo, offset)
  return 16 + tvb(offset + 8, 4):uint()
end

local function dissect_pdu(tvb, pinfo, tree)
  pinfo.cols.protocol = "PODFLOW"
  local plen = tvb(8, 4):uint()
  local t = tree:add(pf, tvb(), "PodFlow")
  t:add(F.magic, tvb(0, 2))
  t:add(F.version, tvb(2, 1))
  t:add(F.msg_type, tvb(3, 1))
  t:add(F.seq, tvb(4, 4))
  t:add(F.len, tvb(8, 4))
  t:add(F.param, tvb(12, 4))
  if plen > 0 then t:add(F.payload, tvb(16, plen)) end
  pinfo.cols.info:set(string.format("%s seq=%d param=%d len=%d",
    types[tvb(3, 1):uint()] or "?", tvb(4, 4):uint(), tvb(12, 4):uint(), plen))
end

function pf.dissector(tvb, pinfo, tree)
  dissect_tcp_pdus(tvb, tree, 16, pdu_len, dissect_pdu)
end

local tcp_port = DissectorTable.get("tcp.port")
tcp_port:add(8888, pf)
for p = 18888, 18899 do tcp_port:add(p, pf) end  -- port yang dipakai skrip tes
```

`dissect_tcp_pdus` menyusun ulang frame yang terpecah di beberapa segmen TCP, sehingga hitungan dan filter jadi akurat.

---

## 8. Daftar screenshot untuk laporan

Simpan di `docs/images/` (empat berkas pertama sudah ada).

| Berkas | Isi | Skenario |
|---|---|---|
| `ws_tcp_handshake.png` | SYN, SYN-ACK, ACK | S1 |
| `ws_hello.png` | `HELLO` dan `WELCOME` | S1 |
| `ws_data_header.png` | Hex dump `DATA seq=1` (`payload_len = 8820`) | S1 |
| `ws_iograph.png` | I/O Graph ~176,7 KB/s | S1 |
| `ws_end_fin.png` | `END (param = 5292044)` dan `FIN` | S1 |
| `ws_pause_gap.png` | Jeda `DATA` saat `PAUSE` (kolom delta) | S2 |
| `ws_seek_ack.png` | `SEEK`, `ACK`, `DATA seq=300` | S3 |
| `ws_stop_noend.png` | `STOP` dan `ACK`, tanpa `END` | S4 |
| `ws_error_200.png` | `ERROR 200` dan koneksi tetap terbuka | S5 |
| `ws_busy_503.png` | `ERROR 503` lalu `FIN` | S6 |
| `ws_conversations_n50.png` | Conversations dan I/O Graph agregat | S7 |
| `ws_zero_window.png` | `zero window` dan pemutusan klien macet | S8 |
| `ws_flowgraph.png` | Flow Graph satu sesi | S1 |
| `ws_websocket_upgrade.png` | `101 Switching Protocols` dan frame WebSocket | S9 |

---

## 9. Pemecahan masalah

| Gejala | Penyebab dan solusi |
|---|---|
| `tcpdump: Permission denied` saat menulis berkas | Tambahkan `-Z "$USER"`, atau jalankan dari folder yang bisa ditulis. |
| Wireshark Windows tidak melihat paket | Rekam di dalam WSL2 (`-i lo`), buka berkas `.pcap` di Windows. Jangan rekam `127.0.0.1` lewat Npcap. |
| Berkas `.pcap` kosong | Rekaman dimulai setelah klien, atau salah port. Cek `-i lo` dan nomor port. |
| Tidak ada paket `FIN` | Rekaman dihentikan terlalu cepat. Tunggu klien keluar dulu. |
| Filter `tcp.payload[...]` tidak menampilkan apa pun | Segmen tidak diawali magic `57 53` (frame terpecah atau digabung). Pakai dissector di bagian 7. |
| Berkas terlalu besar | Pakai `-s 96` untuk skenario banyak klien, atau kurangi N. |
| `Address already in use` saat menyalakan server | Server lama masih berjalan. `pkill server` atau pakai port lain. |
| `S7`: `load.sh` gagal karena port 8888 dipakai | Skrip menyalakan servernya sendiri; hentikan server lain di port 8888. |
| Angka throughput berbeda dari 176,7 KB/s | Laporkan angka terukur; periksa mode daya Windows (Best Performance) dan apakah ada beban lain. |

---

## 10. Urutan kerja yang disarankan

1. S1 (paling penting, memenuhi bagian 5.1 sampai 5.3 laporan).
2. S2, S3, S4 (kontrol pemutaran).
3. S5, S6 (error dan batas klien).
4. S8 (back-pressure), lalu S7 (banyak klien, paling berat).
5. S9 setelah web selesai.
6. Salin semua `.pcap` ke penyimpanan cadangan di luar Git, dan simpan screenshot di `docs/images/`.
