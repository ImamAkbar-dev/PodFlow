# PodFlow Wire Protocol Specification (v1.1)

**Project:** PodFlow Concurrent Audio Streamer (`PodFlow`)  
**Protocol Identifier:** `podflow/1` (`0x5753`, Version `1`)  
**Document Owner:** Member 2 (Protocol, Ring Buffer, Stream Session)  
**Document Version:** `v1.1` (Sessions 2 & 3 Complete — Wire Contract, State Machine, Pacing & Client Error Matrix)  

---

## 1. Overview and Design Goals

The **PodFlow Wire Protocol (v1.1)** is a deterministic, binary, length-prefixed application-layer framing protocol operating over TCP. It enables a multi-threaded C11 server to stream canonical 16-bit, 44.1 kHz stereo PCM `.wav` audio to up to 1,000+ concurrent clients with real-time 50 ms pacing and interactive playback control (`PLAY`, `PAUSE`, `RESUME`, `SEEK`, `STOP`).

### Core Engineering Goals
1. **Deterministic Framing over Byte-Stream TCP:** TCP (`SOCK_STREAM`) is an unstructured byte stream with no native concept of message boundaries; a single `write()` may be split across multiple TCP segments or coalesced with subsequent writes. PodFlow prefixes every frame with a fixed 16-byte header containing an explicit 32-bit `payload_len` field, enabling receivers to reassemble exact frame boundaries via `readn()`.
2. **Strict Endianness & Alignment Safety:** Every multi-byte integer field in the wire header is serialized in **Network Byte Order (Big-Endian)** using POSIX primitives (`htons`, `htonl`, `ntohs`, `ntohl`). The C struct `frame_hdr_t` uses `__attribute__((packed))` and `_Static_assert(sizeof(frame_hdr_t) == 16)` to guarantee zero compiler padding across architectures.
3. **Single-Syscall Atomic Frame Emission:** `send_frame()` marshals the 16-byte header and payload (`<= 65,536` bytes) into a single thread-local stack buffer (`65,552` bytes) and invokes `writen()` once. This prevents splitting header and payload across separate TCP segments, eliminating Nagle's Algorithm + TCP Delayed-ACK latency stalls.
4. **Drift-Free 50 ms Real-Time Pacing:** Outbound audio chunks are paced against `CLOCK_MONOTONIC` at $\Delta t = 50,000,000\text{ ns}$ (50 ms per 8,820-byte chunk = $176,400\text{ B/s}$) without cumulative sleep drift, while never dropping or skipping audio chunks (preserving bit-exact `md5sum` integrity).
5. **Decoupled Disk I/O via Bounded Epoch Ring Buffer:** Each active stream pairs a lazy file-reader producer thread with a 40-slot (2.0 s) ring buffer. `PAUSE` applies natural back-pressure so `RESUME` is instantaneous, while `SEEK` uses an atomic generation `epoch` counter to flush stale audio chunks without lock-order inversion.
6. **Single-Sender Multiplexed Connection Loop:** The per-client connection thread is the sole socket writer and reader, multiplexing incoming client control frames and outbound paced `DATA` frames via `ppoll()`.
7. **Memory Safety & DoS Resilience:** `recv_frame()` validates `magic`, `version`, `payload_len <= MAX_PAYLOAD_SIZE` (`65,536`), and `payload_len <= buf_cap` before reading payload bytes, returning `-4` without draining untrusted bytes. Filenames are sanitized against path traversal, `SO_SNDTIMEO` (5 s) drops stalled readers, and `--max-clients` (default 1,000) sheds excess load cleanly with `ERROR(SERVER_BUSY=503)`.

---

## 2. Transport Layer, Socket Configuration & Connection Lifecycle

* **Transport Protocol:** POSIX stream sockets (`AF_INET`, `SOCK_STREAM`) listening by default on TCP port `8888` (`DEFAULT_PORT`), configurable via `./server [port]`.
* **Concurrency Architecture:** One detached POSIX thread (`pthread`) per accepted client up to `--max-clients N` (default `1000`). When active clients reach the configured cap, incoming connections immediately receive `ERROR(seq=0, param=503, "SERVER_BUSY")` and are closed.
* **`TCP_NODELAY = 1` (`IPPROTO_TCP`):** Disables Nagle's algorithm so 16-byte control frames (`ACK`, `END`) and single-buffer `writen()` calls are emitted immediately onto the wire without waiting for delayed ACKs.
* **`SO_SNDTIMEO = 5` seconds (`SOL_SOCKET`):** Bounds blocking socket writes so a stalled or malicious client that stops draining its TCP receive window is dropped cleanly within 5 seconds.
* **Persistent Multi-Command Connection:**
  * A single TCP connection supports sequential operations (`HELLO` -> `ECHO` -> `LIST` -> `PLAY` -> `END` -> next command) without reconnecting.
  * After `WELCOME`, `ECHO`, `ACK`, `END`, or recoverable command errors (`UNKNOWN_CMD=102`, `FILE_NOT_FOUND=200`, `INVALID_WAV=201`, `SEEK_OUT_OF_RANGE=202`, `INVALID_STATE=300`), the TCP connection **remains open** for the next client command.
  * After fatal framing violations (`BAD_MAGIC=100`, `BAD_VERSION=101`, `PAYLOAD_TOO_LARGE=103`) or load shedding (`SERVER_BUSY=503`), the server transmits `ERROR` (`0xFF`) and **closes the TCP connection immediately**.
  * On clean EOF (`recv_frame()` returns `0`) or mid-frame truncation/socket error (`recv_frame()` returns `-1`), the peer closes the socket descriptor immediately without sending a frame.

---

## 3. Byte-Order & Serialization Rules

1. **Header Multi-Byte Integers:** All 16-bit (`uint16_t`) and 32-bit (`uint32_t`) header fields (`magic`, `seq_num`, `payload_len`, `param`) MUST be encoded in **Big-Endian (Network Byte Order)** via `htons` / `htonl` on transmit and `ntohs` / `ntohl` on receive.
2. **Single-Byte Header Fields:** `version` (`uint8_t`) and `msg_type` (`uint8_t`) occupy 1 byte each at offsets `2` and `3`.
3. **UTF-8 String Payloads:** Text payloads in `HELLO`, `WELCOME` (`"podflow/1"`), `PLAY`, `ACK` (for `LIST`), and `ERROR` are raw **UTF-8 byte sequences without a trailing NUL (`\0`) byte**. Receivers must use `hdr.payload_len` as the exact byte count and append `\0` locally if passing to C string functions.
4. **Binary Audio Payloads (`DATA`):**
   * `DATA` with `seq_num = 0` carries the verbatim 44-byte canonical RIFF/WAVE header from the source file (WAV header fields remain in native RIFF little-endian format).
   * `DATA` with `seq_num >= 1` carries up to `8,820` bytes (`PCM_CHUNK_SIZE`) of raw 16-bit signed little-endian stereo interleaved PCM samples (`S16_LE`) from file offset `44 + (seq_num - 1) * 8820`.

---

## 4. Wire Frame Header Format (16 Bytes)

Every frame transmitted by client or server begins with a packed 16-byte header (`sizeof(frame_hdr_t) == 16`) followed immediately by `payload_len` bytes of payload (`0 <= payload_len <= 65,536`).

### 4.1 RFC-Style 32-Bit Row Diagram

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         magic (0x5753)        |  version (1)  |    msg_type   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            seq_num                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload_len                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             param                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+               payload (0 .. 65,536 bytes, optional)           +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 4.2 Header Field Specification Table

| Offset (Bytes) | Size (Bytes) | C11 Type | Field Name | Wire Endianness | Valid Range / Value | Description & Constraints |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `2` | `uint16_t` | `magic` | Big-Endian | `0x5753` (`"WS"`) | Protocol identifier `0x5753` (ASCII `"WS"`). Mismatch triggers `BAD_MAGIC` (`100`). |
| `2` | `1` | `uint8_t` | `version` | Single Byte | `1` (`PROTO_VERSION`) | Protocol version `1`. Mismatch triggers `BAD_VERSION` (`101`). |
| `3` | `1` | `uint8_t` | `msg_type` | Single Byte | `0x01`..`0x0B`, `0xFF` | Frame opcode from `proto_msg_t`. |
| `4` | `4` | `uint32_t` | `seq_num` | Big-Endian | `0` .. `2^32 - 1` | Sequence number (`0` for WAV header, `1..N` for PCM chunks, `N+1` for `END`, or request ID). |
| `8` | `4` | `uint32_t` | `payload_len` | Big-Endian | `0` .. `65,536` | Payload byte length immediately following byte `15`. Values `> 65,536` trigger `PAYLOAD_TOO_LARGE` (`103`). |
| `12` | `4` | `uint32_t` | `param` | Big-Endian | `0` .. `2^32 - 1` | Context parameter (chunk size, start chunk, target seek chunk, ACK chunk seq, total bytes sent, or error code). |

---

## 5. Message Types Specification

| Opcode | Name | Dir. | `param` Meaning | Payload (`payload_len` B) | Expected Peer Response |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x01` (`1`) | `HELLO` | C -> S | `0` | Optional UTF-8 client name (`0..N` B) | `WELCOME` (`0x02`) |
| `0x02` (`2`) | `WELCOME` | S -> C | `8820` (`PCM_CHUNK_SIZE`) | `"podflow/1"` (`9` B; accept any banner) | None (handshake complete) |
| `0x03` (`3`) | `LIST` | C -> S | `0` | Empty (`0` B) | `ACK` (`0x0A`) with newline-separated `*.wav` names |
| `0x04` (`4`) | `PLAY` (Start) | C -> S | `start_chunk` (`0` or `1` = start; `1..total`) | Target `.wav` filename (`> 0` B, no NUL) | `ACK(eff_start)`, immediate `DATA(seq=0, 44B)`, paced `DATA(seq=n)`, then `END(total+1)` |
| `0x04` (`4`) | `PLAY` (Resume) | C -> S | `0` | Empty (`0` B, valid only in `PAUSED`) | `ACK(param=next_chunk_seq)`, then paced `DATA` resumes |
| `0x05` (`5`) | `PAUSE` | C -> S | `0` | Empty (`0` B, valid only in `PLAYING`) | `ACK(param=next_chunk_seq_to_send)`; sending stops |
| `0x06` (`6`) | `SEEK` | C -> S | `target_chunk` (`1..total_chunks`) | Empty (`0` B, valid in `PLAYING` / `PAUSED`) | Ring flushed, `ACK(target_chunk)`; `DATA` continues from `target_chunk` (WAV header NOT resent) |
| `0x07` (`7`) | `STOP` | C -> S | `0` | Empty (`0` B, valid in `PLAYING` / `PAUSED`) | Ring flushed, `ACK(last_sent_seq)`, state -> `IDLE`, **no `END` frame** |
| `0x08` (`8`) | `DATA` | S -> C | `0` | `seq=0`: 44 B WAV hdr; `seq>=1`: `<= 8,820` B PCM | None (unidirectional stream) |
| `0x09` (`9`) | `END` | S -> C | Total payload bytes sent in stream (incl. 44 B hdr) | Empty (`0` B), `seq = total_chunks + 1` | None; server state returns to `IDLE` |
| `0x0A` (`10`) | `ACK` | S -> C | Confirmed chunk seq (`PLAY`/`PAUSE`/`SEEK`/`STOP`) or `0` | Empty (`0` B) for controls; file list for `LIST` | None |
| `0x0B` (`11`) | `ECHO` | C <-> S | Arbitrary `uint32_t` echoed verbatim | Arbitrary test bytes (`0..65,536` B) | `ECHO` (`0x0B`) with identical `seq_num`, `param`, and payload |
| `0xFF` (`255`) | `ERROR` | S -> C | `proto_err_t` code (`100`..`503`) | UTF-8 diagnostic message (no NUL) | Connection kept or closed per Section 6 |

---

## 6. Error Codes & Client Reaction Specification

An `ERROR` (`0xFF`) frame always carries the numeric `proto_err_t` code in `hdr.param` and a UTF-8 diagnostic string in the payload.

| Code | Name | Trigger Condition | Server Connection Disposition | Mandatory Client Reaction (`client.c`) |
| :--- | :--- | :--- | :--- | :--- |
| `0` | `NONE` | Internal nominal status (never sent in an `ERROR` frame). | **Kept Open** | Continue normal execution; exit `0` when workload completes. |
| `100` | `BAD_MAGIC` | Incoming header `magic != 0x5753` (`recv_frame` returns `-2`). | **Closed Immediately** | Log fatal framing error (`stderr`), close local socket immediately, do not retry on same connection, exit non-zero (`1`). |
| `101` | `BAD_VERSION` | Incoming header `version != 1` (`recv_frame` returns `-3`). | **Closed Immediately** | Log version mismatch (`stderr`), close socket immediately, abort with non-zero exit (`1`). |
| `102` | `UNKNOWN_CMD` | Unrecognized `msg_type` opcode received by server dispatcher. | **Kept Open** (state unchanged) | Log warning with server diagnostic message; connection remains synchronized for subsequent commands. |
| `103` | `PAYLOAD_TOO_LARGE` | Incoming `payload_len > 65536` (`recv_frame` returns `-4` without draining). | **Closed Immediately** | Log fatal oversize payload error (`stderr`), close socket immediately, exit non-zero (`1`). |
| `200` | `FILE_NOT_FOUND` | `PLAY` filename is empty, contains `/` or `..`, or is missing in `./media`. | **Kept Open** (`IDLE`) | Log file-not-found error; close and `unlink()` any partial output file so no 0-byte file remains. Connection stays usable; exit non-zero (`2`) in CLI mode. |
| `201` | `INVALID_WAV` | Target file is `<= 44` bytes or lacks canonical 16-bit 44.1 kHz stereo PCM header. | **Kept Open** (`IDLE`) | Log invalid media format error, remove any partial output file (`unlink`), keep connection open (or exit `2` in CLI mode). |
| `202` | `SEEK_OUT_OF_RANGE` | `PLAY start_chunk > total_chunks` or `SEEK` target not in `1..total_chunks`. | **Kept Open** (state unchanged) | Log seek range warning; retain existing playback state/buffer and clamp seek requests to `[1 .. total_chunks]`. |
| `300` | `INVALID_STATE` | Command invalid in current state (`PAUSE` outside `PLAYING`, `SEEK`/`STOP` in `IDLE`, `PLAY(file)` outside `IDLE`, resume outside `PAUSED`). | **Kept Open** (state unchanged) | Log state error message; connection remains open and synchronized for valid commands. |
| `500` | `INTERNAL_SERVER` | Unexpected OS, disk `fseeko`, or thread allocation error on server. | **Kept Open** (or closed on I/O fault) | Log server fault, discard incomplete output file, close or reconnect with backoff, exit non-zero (`3`). |
| `503` | `SERVER_BUSY` | Active client count reached `--max-clients` limit upon connection accept. | **Closed Immediately** | Log server-busy error, close socket immediately, optionally retry with exponential backoff. |

---

## 7. Per-Connection State Machine

Each client connection maintains an independent state machine initialized to `IDLE` upon TCP connection establishment.

```text
                       +-----------------------------------+
                       |               IDLE                |
                       +-----------------------------------+
                         |                               ^
   PLAY(file, start)     |                               | 1. STOP -> ACK(last_sent_seq)
   -> ACK(eff_start)     |                               | 2. Stream EOF -> END(total_chunks+1,
   -> DATA(seq=0, 44B)   |                               |                    param=total_bytes_sent)
                         v                               |
                       +-----------------------------------+
        +------------->|              PLAYING              |<-------------+
        |              +-----------------------------------+              |
        |                |                               ^                |
  SEEK(target)           | PAUSE                         | PLAY("")       | SEEK(target)
  -> rb_flush, fseeko    | -> ACK(next_seq)              | (resume)       | -> rb_flush, fseeko
  -> ACK(target)         v                               | -> ACK(next)   | -> ACK(target)
        |              +-------------------+             |                |
        +--------------|      PAUSED       |-------------+----------------+
                       +-------------------+
                         |               |
                         +-------+-------+
                                 |
                                 | STOP -> ACK(last_sent_seq) [No END]
                                 v
                         [ Returns to IDLE ]
```

### 7.1 State × Command Transition Matrix

| Command \ State | `IDLE` | `PLAYING` | `PAUSED` |
| :--- | :--- | :--- | :--- |
| **`HELLO`** | `WELCOME(8820, "podflow/1")` -> `IDLE` | `WELCOME` -> `PLAYING` | `WELCOME` -> `PAUSED` |
| **`ECHO`** | `ECHO` (mirror `seq`, `param`, `data`) -> `IDLE` | `ECHO` -> `PLAYING` | `ECHO` -> `PAUSED` |
| **`LIST`** | `ACK(filenames)` -> `IDLE` | `ACK(filenames)` -> `PLAYING` | `ACK(filenames)` -> `PAUSED` |
| **`PLAY(filename, start)`** | Valid: `ACK(eff_start)`, `DATA(0)` -> `PLAYING`<br>Bad file: `ERROR(200/201)` -> `IDLE`<br>Range: `ERROR(202)` -> `IDLE` | `ERROR(300 INVALID_STATE)` -> `PLAYING` | `ERROR(300 INVALID_STATE)` -> `PAUSED` |
| **`PLAY("", 0)` (Resume)** | `ERROR(300 INVALID_STATE)` -> `IDLE` | `ERROR(300 INVALID_STATE)` -> `PLAYING` | `ACK(next_seq)`, `pacer_reset` -> `PLAYING` |
| **`PAUSE`** | `ERROR(300 INVALID_STATE)` -> `IDLE` | `ACK(next_seq)` -> `PAUSED` | `ERROR(300 INVALID_STATE)` -> `PAUSED` |
| **`SEEK(target)`** | `ERROR(300 INVALID_STATE)` -> `IDLE` | `1..total`: flush, `ACK(target)`, `pacer_reset` -> `PLAYING`<br>Out of range: `ERROR(202)` -> `PLAYING` | `1..total`: flush, `ACK(target)` -> `PAUSED`<br>Out of range: `ERROR(202)` -> `PAUSED` |
| **`STOP`** | `ERROR(300 INVALID_STATE)` -> `IDLE` | Flush, close file, `ACK(last_sent_seq)` -> `IDLE` | Flush, close file, `ACK(last_sent_seq)` -> `IDLE` |

---

## 8. Protocol Sequence Diagrams

### 8.1 Handshake (`HELLO` / `WELCOME`) and `ECHO`

```text
Client                                                       Server
  |--- HELLO (type=1, seq=0, param=0, len=6, "client") ------->|
  |<-- WELCOME (type=2, seq=0, param=8820, len=9, "podflow/1")-|
  |--- ECHO (type=11, seq=1, param=123, len=32, <bytes>) ----->|
  |<-- ECHO (type=11, seq=1, param=123, len=32, <bytes>) ------|
```

### 8.2 Full Paced Stream (`PLAY` -> `DATA` ... -> `END`)

Illustrated for `sample_30s.wav` ($5,292,044\text{ bytes} = 44\text{-byte header} + 600 \times 8,820\text{-byte PCM chunks}$):

```text
Client                                                       Server (State: IDLE)
  |--- PLAY (type=4, seq=1, param=0, "sample_30s.wav") ------->|
  |<-- ACK  (type=10, seq=1, param=1, len=0) ------------------| [State -> PLAYING, pacer_reset()]
  |<-- DATA (type=8, seq=0, param=0, len=44, <WAV Header>) ----| [Immediate (t = 0 ms)]
  |<-- DATA (type=8, seq=1, param=0, len=8820, <PCM #1>) ------| [Immediate first chunk (t = 0 ms)]
  |    ... (50 ms CLOCK_MONOTONIC interval) ...                |
  |<-- DATA (type=8, seq=2, param=0, len=8820, <PCM #2>) ------| [t = 50 ms]
  |<-- DATA (type=8, seq=600, param=0, len=8820, <PCM #600>) --| [t = 29,950 ms]
  |<-- END  (type=9, seq=601, param=5292044, len=0) -----------| [State -> IDLE]
```

### 8.3 Interactive `PAUSE` and `RESUME`

```text
Client                                                       Server (State: PLAYING)
  |<-- DATA  (type=8, seq=20, param=0, len=8820) --------------|
  |--- PAUSE (type=5, seq=2, param=0, len=0) ----------------->|
  |<-- ACK   (type=10, seq=2, param=21, len=0) ----------------| [State -> PAUSED; ring keeps filling]
  |    ... (Paused; no DATA frames transmitted) ...            |
  |--- PLAY  (type=4, seq=3, param=0, len=0) ----------------->| [Empty payload = RESUME]
  |<-- ACK   (type=10, seq=3, param=21, len=0) ----------------| [State -> PLAYING, pacer_reset()]
  |<-- DATA  (type=8, seq=21, param=0, len=8820) --------------| [Immediate first chunk after resume]
```

### 8.4 `SEEK` During Active Playback (With Stale-`DATA` Filter Rule)

```text
Client                                                       Server (State: PLAYING)
  |<-- DATA (type=8, seq=10, param=0, len=8820) ---------------|
  |--- SEEK (type=6, seq=4, param=100, len=0) ---------------->|
  |<-- DATA (type=8, seq=11, param=0, len=8820) [IN-FLIGHT] ---| [Client ignores: arrives before ACK]
  |                                                            | [fseeko(chunk 100), rb_flush()]
  |<-- ACK  (type=10, seq=4, param=100, len=0) ----------------| [pacer_reset()]
  |<-- DATA (type=8, seq=100, param=0, len=8820) --------------| [Client accepts: WAV hdr NOT resent]
```

### 8.5 `STOP`, Wrong-State Error (`300`), and `SERVER_BUSY` (`503`)

```text
Client                                                       Server (State: PLAYING)
  |--- STOP  (type=7, seq=5, param=0, len=0) ----------------->|
  |<-- ACK   (type=10, seq=5, param=35, len=0) ----------------| [State -> IDLE; NO END frame sent]
  |--- PAUSE (type=5, seq=6, param=0, len=0) ----------------->| [Invalid in IDLE]
  |<-- ERROR (type=0xFF, seq=6, param=300, "INVALID_STATE") ---| [Connection KEPT OPEN]

Client #1001 (--max-clients 1000 reached)                    Server
  |--- [TCP Connect] ----------------------------------------->|
  |<-- ERROR (type=0xFF, seq=0, param=503, "SERVER_BUSY") -----| [Closed immediately]
```

### 8.6 `PLAY` Error (`FILE_NOT_FOUND = 200`) & Fatal Violation (`BAD_MAGIC = 100`)

```text
Client                                                       Server (State: IDLE)
  |--- PLAY (type=4, seq=1, param=0, len=11, "missing.wav") -->|
  |<-- ERROR (type=0xFF, seq=1, param=200, "FILE_NOT_FOUND") --| [Connection KEPT OPEN]

Client                                                       Server
  |--- [Frame with magic=0xDEAD, ver=1, seq=4, len=32] ------->| [recv_frame() -> -2]
  |<-- ERROR (type=0xFF, seq=4, param=100, "BAD_MAGIC") -------|
  |<-- [Server closes socket immediately (TCP FIN)] -----------|
```

---

## 9. Audio Chunk Math, Seek Formula, Pacing and Stale-DATA Rules

### 9.1 Exact Derivation of `PCM_CHUNK_SIZE = 8820`
Canonical PodFlow audio uses 16-bit (2 B), 44,100 Hz, 2-channel stereo PCM:
* **BlockAlign ($B_{\text{align}}$):** $2\text{ channels} \times 2\text{ bytes} = 4\text{ bytes per stereo sample frame}$.
* **ByteRate ($R_{\text{byte}}$):** $44,100\text{ Hz} \times 4\text{ bytes} = 176,400\text{ bytes/second}$.
* **`PCM_CHUNK_SIZE` (50 ms):** $176,400\text{ B/s} \times 0.05\text{ s} = 8,820\text{ bytes}$ ($2,205$ stereo sample frames; $8,820 \bmod 4 = 0$, preventing channel tearing).
* **Total Chunks (file size $F > 44$):**
  $$\text{total\_chunks} = \left\lceil \frac{F - 44}{8820} \right\rceil = \frac{(F - 44) + 8819}{8820}$$

### 9.2 Reference Media Test Suite Sizes
1. **`sample_30s.wav` ($30.00\text{ s}$):** $44 + (600 \times 8,820) = 5,292,044\text{ bytes}$ (`END.seq_num = 601`, `END.param = 5292044`).
2. **`sample_5m.wav` ($300.00\text{ s}$):** $44 + (6,000 \times 8,820) = 52,920,044\text{ bytes}$ (`END.seq_num = 6001`, `END.param = 52920044`).
3. **`sample_tail.wav` ($2.37\text{ s}$ — Partial Tail Chunk):** $44 + (47 \times 8,820) + 3,528 = 418,112\text{ bytes}$ (`47` full chunks + `1` partial tail chunk of $3,528\text{ B}$; `END.seq_num = 49`, `END.param = 418112`).

### 9.3 Seek Offset Formula and Worked Examples
**File Byte Offset for 1-Based Chunk $n$ ($1 \le n \le \text{total\_chunks}$):**
$$\text{offset}(n) = 44 + (n - 1) \times 8820$$

* **Chunk $n = 1$ (Start of PCM):** $\text{offset}(1) = 44 + (1 - 1) \times 8820 = 44\text{ bytes}$.
* **Chunk $n = 100$ (5.0-second mark in `sample_30s.wav`):** $\text{offset}(100) = 44 + 99 \times 8820 = 873,224\text{ bytes}$.
* **Chunk $n = 600$ (Final chunk of `sample_30s.wav`, $F = 5,292,044$):** $\text{offset}(600) = 44 + 599 \times 8820 = 5,283,224\text{ bytes}$ (leaving $8,820\text{ bytes}$).

### 9.4 Pacing Timeline, Late-Reset Policy and Stale-DATA Rule
* **Absolute Monotonic Timeline:** `pacer_reset()` sets `next_tick = now` (`CLOCK_MONOTONIC`) upon `PLAY`, `RESUME`, or `SEEK`, so the first chunk is emitted immediately without an initial 50 ms delay. Subsequent ticks advance strictly by `next_tick += 50,000,000 ns`.
* **Late-Reset Policy (`> 5` Intervals = 250 ms):** If OS scheduling or TCP back-pressure causes lateness to exceed 5 intervals (250 ms), `pacer_tick_done()` increments `late_resets++` and resets `next_tick = now + 50 ms` to prevent a catch-up burst. **No chunk is ever skipped or dropped**, preserving bit-exact `md5sum` integrity.
* **Client Stale-DATA Discard Rule:** After transmitting `PAUSE`, `SEEK`, or `STOP`, any `DATA` frames already in flight in the TCP socket buffer prior to the server's `ACK` are stale. Clients MUST ignore incoming `DATA` frames until the corresponding `ACK` frame is received.

---

## 10. Security Notes, Limits and Versioning Policy

* **Filename Validation & Path Traversal Defense:** `session_play()` rejects empty names, lengths `>= 256`, embedded NUL bytes, `'/'`, `'\\'`, `"."`, and `".."`, and enforces `S_ISREG(st.st_mode)` inside `./media` before opening.
* **Bounded Frame Allocation:** `recv_frame()` enforces `payload_len <= 65536` (`MAX_PAYLOAD_SIZE`) and `payload_len <= buf_cap` prior to reading payload bytes, returning `-4` without draining untrusted bytes so the server sends `ERROR(103)` and closes immediately.
* **Versioning Policy (v1 vs. v2):** Adding new opcodes, error codes (such as `503 SERVER_BUSY`), or banner text preserves the 16-byte wire header layout and remains compatible under `PROTO_VERSION = 1`. Altering header field offsets, widths, or audio codec framing requires incrementing `PROTO_VERSION` to `2`.

---

## 11. Worked Hex Dumps

All multi-byte header fields below are encoded in **Big-Endian (Network Byte Order)**:

1. **`HELLO` Frame:** `filename/client = "cli-1"` (5 B), `seq = 0`, `param = 0` (21 B total)
```text
0000: 57 53 01 01 00 00 00 00  00 00 00 05 00 00 00 00  WS..............
0010: 63 6C 69 2D 31                                    cli-1
```

2. **`PLAY` Frame:** `filename = "sample_30s.wav"` (14 B), `start_chunk = 1`, `seq = 1` (30 B total)
```text
0000: 57 53 01 04 00 00 00 01  00 00 00 0E 00 00 00 01  WS..............
0010: 73 61 6D 70 6C 65 5F 33  30 73 2E 77 61 76        sample_30s.wav
```

3. **`ACK` Frame:** `seq = 1`, `param = 1`, `payload_len = 0` (16 B total)
```text
0000: 57 53 01 0A 00 00 00 01  00 00 00 00 00 00 00 01  WS..............
```

4. **`SEEK` Frame:** `seq = 4`, `target = 100` (`0x00000064`), `payload_len = 0` (16 B total)
```text
0000: 57 53 01 06 00 00 00 04  00 00 00 00 00 00 00 64  WS.............d
```

5. **`DATA` Frame Header:** `seq = 100` (`0x64`), `payload_len = 8820` (`0x2274`), `param = 0` (16 B hdr + 8,820 B PCM)
```text
0000: 57 53 01 08 00 00 00 64  00 00 22 74 00 00 00 00  WS.....d.."t....
```

---

## 12. Change Log (`v1.0` -> `v1.1`)

| Version | Phase | Author | Summary of Changes |
| :--- | :--- | :--- | :--- |
| `v1.0` | Session 1 | Member 2 | Initial 16-byte big-endian framing (`0x5753`), `readn`/`writen`, `HELLO`/`WELCOME`/`ECHO`/`LIST`, unpaced `PLAY` (`DATA`..`END`), and error codes `100`–`500`. |
| `v1.1` | Sessions 2 & 3 | Member 2 | Added `IDLE`/`PLAYING`/`PAUSED` state machine, `PLAY` `start_chunk` & empty-payload `RESUME`, `PAUSE`/`SEEK`/`STOP` semantics with `ACK(param)`, 50 ms `CLOCK_MONOTONIC` pacing & `> 250 ms` late-reset rules, stale-`DATA` discard rule, mandatory client error-reaction matrix, `ERR_SERVER_BUSY` (`503`), and updated banner to `"podflow/1"`. |
