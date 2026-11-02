# PodFlow Wire Protocol Specification (v1.0)

**Project:** Concurrent Chunked Audio/Media Streaming Server (`wav-streamer` / `PodFlow`)  
**Protocol Identifier:** `wav-streamer/1` (`0x5753`, Version `1`)  
**Document Owner:** Member 2 (Protocol & Framing Lead)  
**Status:** Frozen for Session 1 / Active Reference for Session 2  

---

## 1. Overview and Design Goals

The **PodFlow Wire Protocol (WS v1)** is a deterministic, binary, length-prefixed application-layer framing protocol operating over TCP. It is designed to transport canonical 16-bit 44.1 kHz stereo PCM `.wav` audio streams from a multi-threaded POSIX C11 server to concurrent clients.

### Core Design Goals
1. **Explicit Message Framing Over Byte-Stream TCP:** TCP provides an unstructured, connection-oriented byte stream without inherent message boundaries. A single `send_frame()` call may be fragmented across multiple TCP segments or coalesced with subsequent frames. WS v1 prefixes every message with a fixed 16-byte header containing an explicit `payload_len` field so receivers can deterministically reconstruct exact frame boundaries via `readn()`.
2. **Zero-Ambiguity Endianness and Alignment:** All multi-byte integer fields in the wire header are encoded in **Network Byte Order (Big-Endian)** using standard POSIX conversion primitives (`htons`, `htonl`, `ntohs`, `ntohl`).
3. **Single-Syscall Frame Emission:** Header and payload bytes are marshaled into a contiguous stack buffer (`16 + 65,536` bytes max) and written in a single `writen()` operation to prevent Nagle's algorithm and TCP Delayed-ACK latency stalls.
4. **Bounded Memory & Overflow Immunity:** Payloads are strictly bounded to `MAX_PAYLOAD_SIZE` (65,536 bytes). Receivers validate `magic`, `version`, and `payload_len` before reading payload bytes, preventing buffer overflows and unbounded memory allocation attacks.
5. **Forward Extensibility for Playback Control:** The fixed `param` header field and reserved control opcodes (`PAUSE`, `SEEK`, `STOP`) allow Session 2 real-time pacing and random-access chunk seeking without changing the 16-byte wire header layout.

---

## 2. Transport Layer & Connection Model

* **Transport Protocol:** TCP (`SOCK_STREAM`, `AF_INET`).
* **Default Server Port:** `8888` (configurable via CLI `./server [port]`).
* **Concurrency Model:** One dedicated POSIX thread (`pthread`) per accepted client connection in Session 1.
* **Connection Persistence:** A single TCP connection supports multiple sequential request-response transactions (`HELLO` -> `ECHO` -> `LIST` -> `PLAY` -> `END` -> next command). The server keeps the connection open after `END` and non-fatal command errors (`UNKNOWN_CMD`, `FILE_NOT_FOUND`, `INVALID_WAV`, `INVALID_STATE`). The client initiates normal termination by closing the socket after completing its workload.

---

## 3. Byte-Order & Serialization Rules

1. **Header Multi-Byte Integers:** All 16-bit (`uint16_t`) and 32-bit (`uint32_t`) fields in `frame_hdr_t` MUST be transmitted in **Big-Endian (Network Byte Order)**—most significant byte first.
2. **Single-Byte Header Fields:** `version` (`uint8_t`) and `msg_type` (`uint8_t`) occupy 1 byte each at offsets 2 and 3.
3. **Text Payloads:** String payloads (filenames in `PLAY`, directory listings in `ACK`, banners in `WELCOME`, diagnostic messages in `ERROR`) are encoded as raw **UTF-8 bytes WITHOUT a trailing NUL (`\0`) terminator**. The exact string length is given by `payload_len`.
4. **Audio Payloads (`DATA` frames):**
   * `DATA` with `seq_num = 0` carries the raw 44-byte RIFF/WAVE header verbatim from the source file (WAV header fields themselves follow the RIFF specification's native little-endian layout).
   * `DATA` with `seq_num >= 1` carries raw 16-bit signed little-endian stereo interleaved PCM audio samples (`S16_LE`, 2 channels) verbatim from the source `.wav` file.

---

## 4. Wire Frame Header Format (16 Bytes)

Every frame transmitted by either client or server begins with a 16-byte header followed immediately by `payload_len` bytes of payload (`0 <= payload_len <= 65536`).

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

| Byte Offset | Size (Bytes) | C Type | Field Name | Wire Byte Order | Description & Constraints |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `2` | `uint16_t` | `magic` | Big-Endian | Protocol identifier magic number. Must equal `0x5753` (ASCII `'W'` `0x57`, `'S'` `0x53`). |
| `2` | `1` | `uint8_t` | `version` | N/A (1 byte) | Protocol version number. Must equal `1` (`PROTO_VERSION`). |
| `3` | `1` | `uint8_t` | `msg_type` | N/A (1 byte) | Frame opcode defined in `proto_msg_t` (`0x01`..`0x0B`, `0xFF`). |
| `4` | `4` | `uint32_t` | `seq_num` | Big-Endian | Monotonic sequence number or request correlation ID. |
| `8` | `4` | `uint32_t` | `payload_len` | Big-Endian | Number of payload bytes immediately following byte 15. Valid range: `0` to `65536` (`MAX_PAYLOAD_SIZE`). |
| `12` | `4` | `uint32_t` | `param` | Big-Endian | Context-dependent 32-bit parameter (e.g., chunk size in `WELCOME`, total bytes in `END`, error code in `ERROR`). |

---

## 5. Message Types Specification

| Opcode (Hex) | Name | Direction | `seq_num` Meaning | `param` Meaning | Payload Content (`payload_len` bytes) | Expected Peer Response |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `0x01` | `HELLO` | Client -> Server | `0` | `0` (unused) | Optional UTF-8 client identifier string (e.g., `"cli-1"`), or empty (`0` bytes). | `WELCOME` (`0x02`) |
| `0x02` | `WELCOME` | Server -> Client | `0` | `8820` (`PCM_CHUNK_SIZE`) | Server banner string `"wav-streamer/1"` (14 bytes, no NUL). | None (handshake complete) |
| `0x03` | `LIST` | Client -> Server | Client-chosen (e.g., `0`) | `0` | Empty (`payload_len = 0`). | `ACK` (`0x0A`) containing newline-separated `.wav` filenames in `./media`. |
| `0x04` | `PLAY` | Client -> Server | `0` | Session 1: `0`. Session 2: start chunk index (`0` = start). | Target `.wav` filename inside `./media` (UTF-8, no path traversal, no NUL). | Stream of `DATA` (`0x08`) frames (`seq=0..N`) followed by `END` (`0x09`) (`seq=N+1`), or `ERROR` (`200`/`201`). |
| `0x05` | `PAUSE` | Client -> Server | Client-chosen | `0` | Empty (`payload_len = 0`). | Session 1: `ERROR` `INVALID_STATE` (`300`). Session 2: `ACK` (`0x0A`). |
| `0x06` | `SEEK` | Client -> Server | Client-chosen | Target chunk number (`>= 1`). | Empty (`payload_len = 0`). | Session 1: `ERROR` `INVALID_STATE` (`300`). Session 2: `DATA` stream from target chunk or `ERROR` (`202`). |
| `0x07` | `STOP` | Client -> Server | Client-chosen | `0` | Empty (`payload_len = 0`). | Session 1: `ERROR` `INVALID_STATE` (`300`). Session 2: `END` (`0x09`) or `ACK` (`0x0A`). |
| `0x08` | `DATA` | Server -> Client | `0` = WAV header; `1..N` = PCM chunk index. | `0` | `seq=0`: 44-byte WAV header. `seq=1..N`: `<= 8820` bytes of raw PCM audio. | None (unidirectional stream until `END`). |
| `0x09` | `END` | Server -> Client | `N + 1` (where `N` is last `DATA` seq) | Total payload bytes sent across `seq=0..N` (including 44-byte WAV header). | Empty (`payload_len = 0`). | None (connection stays open for next command). |
| `0x0A` | `ACK` | Server -> Client | Matches request `seq_num` | Context-dependent (`0` for `LIST`). | For `LIST`: newline-separated `*.wav` filenames in `./media`. | None. |
| `0x0B` | `ECHO` | Bidirectional | Arbitrary `i` (`uint32_t`) | Arbitrary `p` (`uint32_t`) | Arbitrary test bytes (`0..65536` bytes). | Server replies with `ECHO` (`0x0B`) preserving identical `seq_num`, `param`, and payload. |
| `0xFF` | `ERROR` | Server -> Client | Offending request `seq_num` (or `0`) | `proto_err_t` error code (`100`..`500`). | Human-readable UTF-8 diagnostic message (no NUL). | Connection closed on `100`, `101`, `103`; kept open on `102`, `200`, `201`, `202`, `300`, `500`. |

---

## 6. Error Codes Specification

When an error occurs, the server transmits an `ERROR` (`0xFF`) frame with `hdr.param` set to the numeric `proto_err_t` value and the payload containing a UTF-8 description.

| Code (Dec) | Symbolic Name | Category | Meaning | Trigger Condition | Connection Disposition |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `NONE` | Nominal | No error | Internal success status | Kept Open |
| `100` | `BAD_MAGIC` | Framing / Protocol | Header `magic != 0x5753` | `recv_frame()` returns `-2` | **Closed Immediately** after sending `ERROR` |
| `101` | `BAD_VERSION` | Framing / Protocol | Header `version != 1` | `recv_frame()` returns `-3` | **Closed Immediately** after sending `ERROR` |
| `102` | `UNKNOWN_CMD` | Command / Protocol | Unrecognized `msg_type` opcode | Server switch default branch | **Kept Open** for subsequent commands |
| `103` | `PAYLOAD_TOO_LARGE` | Framing / Protocol | `payload_len > 65536` (or exceeds receiver cap) | `recv_frame()` returns `-4` | **Closed Immediately** after sending `ERROR` (payload not drained) |
| `200` | `FILE_NOT_FOUND` | Media / Resource | Requested `.wav` file does not exist in `./media` | `open()` fails with `ENOENT` or path rejected | **Kept Open** for subsequent commands |
| `201` | `INVALID_WAV` | Media / Format | File is smaller than 44 bytes or lacks valid RIFF/WAVE PCM header | WAV header validation fails on `PLAY` | **Kept Open** for subsequent commands |
| `202` | `SEEK_OUT_OF_RANGE` | Playback Control | Target seek chunk exceeds total chunks in file or is `0` | `SEEK` validation in Session 2 | **Kept Open** |
| `300` | `INVALID_STATE` | State Machine | Command not valid in current session/state | `PAUSE`, `SEEK`, or `STOP` sent in Session 1 | **Kept Open** for subsequent commands |
| `500` | `INTERNAL_SERVER` | Server Internal | Unexpected system error (`read` failure, `malloc` failure) | Disk I/O or OS error on server | **Kept Open** (or closed if stream already started) |

---

## 7. Protocol Sequence Diagrams

### 7.1 Handshake (`HELLO` / `WELCOME`)

```text
Client                                                       Server
  |                                                            |
  |--- [TCP SYN / SYN-ACK / ACK (Port 8888)] ----------------->|
  |                                                            |
  |--- HELLO (type=0x01, seq=0, param=0, len=5, "cli-1") ----->|
  |                                                            |
  |<-- WELCOME (type=0x02, seq=0, param=8820,                  |
  |             len=14, "wav-streamer/1") ---------------------|
  |                                                            |
```

### 7.2 Echo Verification Loop (`ECHO`)

```text
Client                                                       Server
  |                                                            |
  |--- ECHO (type=0x0B, seq=1, param=42, len=128, <bytes>) --->|
  |<-- ECHO (type=0x0B, seq=1, param=42, len=128, <bytes>) ----|
  |                                                            |
  |--- ECHO (type=0x0B, seq=2, param=99, len=8820, <bytes>) -->|
  |<-- ECHO (type=0x0B, seq=2, param=99, len=8820, <bytes>) ---|
  |                                                            |
```

### 7.3 Successful Audio Stream (`PLAY` -> `DATA`... -> `END`)

Example shown for `sample_30s.wav` ($5,292,044$ bytes total: $44$-byte WAV header + $600 \times 8,820$-byte PCM chunks):

```text
Client                                                       Server
  |                                                            |
  |--- PLAY (type=0x04, seq=0, param=0,                        |
  |          len=14, "sample_30s.wav") ----------------------->|
  |                                                            | [Opens ./media/sample_30s.wav]
  |<-- DATA (type=0x08, seq=0,   param=0, len=44, <WAV hdr>) --| [Validates 44-byte RIFF header]
  |<-- DATA (type=0x08, seq=1,   param=0, len=8820, <PCM>) ----|
  |<-- DATA (type=0x08, seq=2,   param=0, len=8820, <PCM>) ----|
  |    ...                                                     |
  |<-- DATA (type=0x08, seq=600, param=0, len=8820, <PCM>) ----|
  |<-- END  (type=0x09, seq=601, param=5292044, len=0) --------|
  |                                                            | [Connection remains OPEN]
```

### 7.4 Failed Play Request (`FILE_NOT_FOUND`)

```text
Client                                                       Server
  |                                                            |
  |--- PLAY (type=0x04, seq=0, param=0,                        |
  |          len=11, "missing.wav") -------------------------->|
  |                                                            | [open("./media/missing.wav") -> ENOENT]
  |<-- ERROR (type=0xFF, seq=0, param=200,                     |
  |           len=14, "File not found") -----------------------|
  |                                                            | [Connection remains OPEN]
```

### 7.5 Fatal Protocol Violation (`BAD_MAGIC` / `PAYLOAD_TOO_LARGE`)

```text
Client                                                       Server
  |                                                            |
  |--- [Malformed Frame: magic=0xDEAD, seq=7, len=100] ------->|
  |                                                            | [recv_frame() -> -2 (RECV_ERR_BAD_MAGIC)]
  |<-- ERROR (type=0xFF, seq=7, param=100,                     |
  |           len=9, "BAD_MAGIC") -----------------------------|
  |<-- [TCP FIN / Close Connection Immediately] ---------------|
  |                                                            |
```

---

## 8. Audio Chunk-Size Derivation & WAV Header Handling

### 8.1 Mathematical Derivation of `PCM_CHUNK_SIZE = 8820`
All reference audio files in PodFlow use standard CD-quality uncompressed PCM parameters:
* **Sample Rate ($f_s$):** $44,100\text{ Hz}$ (samples per second per channel)
* **Channels ($C$):** $2$ (Stereo: Left + Right)
* **Bit Depth ($b$):** $16\text{ bits/sample} = 2\text{ bytes/sample}$
* **BlockAlign ($B_{\text{align}}$):** $C \times (b / 8) = 2 \times 2 = 4\text{ bytes per stereo sample frame}$
* **ByteRate ($R_{\text{byte}}$):** $f_s \times B_{\text{align}} = 44,100 \times 4 = 176,400\text{ bytes/second}$

For a target chunk duration of $\Delta t = 50\text{ ms} = 0.05\text{ seconds}$ (20 chunks per second):
$$\text{PCM\_CHUNK\_SIZE} = R_{\text{byte}} \times \Delta t = 176,400\text{ B/s} \times 0.05\text{ s} = 8,820\text{ bytes}$$

**Frame Alignment Verification:**
$$\frac{8,820\text{ bytes}}{4\text{ bytes/stereo frame}} = 2,205\text{ stereo sample frames (exact integer)}$$
Because $8,820$ is evenly divisible by $\text{BlockAlign} = 4$, no stereo sample frame is ever split across two `DATA` frames.

### 8.2 Reference Test Files & Partial Tail Chunks
* **`sample_30s.wav` ($30.00\text{ s}$):** $44 + (600 \times 8,820) = 5,292,044\text{ bytes}$. Sent as `DATA(seq=0, len=44)`, `DATA(seq=1..600, len=8820)`, `END(seq=601, param=5292044)`.
* **`sample_5m.wav` ($300.00\text{ s}$):** $44 + (6000 \times 8,820) = 52,920,044\text{ bytes}$. Sent as `DATA(seq=0, len=44)`, `DATA(seq=1..6000, len=8820)`, `END(seq=6001, param=52920044)`.
* **`sample_tail.wav` ($2.37\text{ s}$):** $2.37\text{ s} \times 176,400\text{ B/s} = 418,068\text{ bytes of PCM} = (47 \times 8,820) + 3,528\text{ bytes}$. Total file size $= 44 + 418,068 = 418,112\text{ bytes}$. Sent as `DATA(seq=0, len=44)`, `DATA(seq=1..47, len=8820)`, `DATA(seq=48, len=3528)`, `END(seq=49, param=418112)`.

### 8.3 Canonical 44-Byte WAV Header Handling (`seq = 0`)
Upon receiving `PLAY`, the server reads the first 44 bytes of the target `.wav` file, verifies `"RIFF"` at offset 0, `"WAVE"` at offset 8, AudioFormat `1` (PCM), NumChannels `2`, SampleRate `44100`, and BitsPerSample `16`, and transmits those 44 bytes unaltered in `DATA` with `seq_num = 0`. A client saving the stream (`./client get`) simply writes the payload of `DATA(seq=0)` followed by `DATA(seq=1..N)` sequentially to the output file to produce a bit-identical copy (`md5sum` match) of the server's `.wav` file.

---

## 9. Protocol Limits & Versioning Policy

### 9.1 Hard Protocol Limits
* `FRAME_HEADER_SIZE`: `16` bytes.
* `MAX_PAYLOAD_SIZE`: `65,536` bytes ($64\text{ KiB}$). Any frame header declaring `payload_len > 65536` is immediately rejected by `recv_frame()` with `RECV_ERR_PAYLOAD_LEN` (`-4`).
* Maximum Wire Frame Size: $16 + 65,536 = 65,552\text{ bytes}$.

### 9.2 Versioning Policy
* Version `1` (`PROTO_VERSION = 1`) is the sole version defined for Sessions 1 and 2.
* Backward-compatible extensions within Version `1` must use previously reserved opcodes (`PAUSE=0x05`, `SEEK=0x06`, `STOP=0x07`) or reserved `param` values (`PLAY` start chunk).
* Any change to the 16-byte header size, field offsets, endianness, or magic number requires incrementing `PROTO_VERSION` to `2`. Peers receiving a version other than `1` MUST return `RECV_ERR_BAD_VERSION` (`-3`), emit `ERROR(BAD_VERSION=101)`, and close the connection.

---

## 10. Worked Hex Dumps

### 10.1 Worked Example 1: `HELLO` Frame (21 Bytes Total)
**Scenario:** Client sends `HELLO` (`msg_type = 0x01`), `seq_num = 0`, `param = 0`, payload = `"cli-1"` (5 ASCII bytes: `0x63 0x6C 0x69 0x2D 0x31`).

```text
Byte Offset   Hex Bytes                  Field Breakdown
-----------   -----------------------    ----------------------------------------------
0000 .. 0001  57 53                      magic       = 0x5753 ("WS", Big-Endian)
0002          01                         version     = 0x01 (1)
0003          01                         msg_type    = 0x01 (HELLO)
0004 .. 0007  00 00 00 00                seq_num     = 0x00000000 (0, Big-Endian)
0008 .. 000B  00 00 00 05                payload_len = 0x00000005 (5 bytes, Big-Endian)
000C .. 000F  00 00 00 00                param       = 0x00000000 (0, Big-Endian)
0010 .. 0014  63 6C 69 2D 31             payload     = "cli-1" (5 bytes, no NUL)
```

### 10.2 Worked Example 2: `DATA` Frame Header for `seq = 1`, `payload_len = 8820` (16-Byte Header)
**Scenario:** Server sends the first PCM chunk: `msg_type = 0x08` (`DATA`), `seq_num = 1`, `payload_len = 8820` (`8820` in decimal $= 0\text{x}00002274$ in hexadecimal), `param = 0`.

```text
Byte Offset   Hex Bytes                  Field Breakdown
-----------   -----------------------    ----------------------------------------------
0000 .. 0001  57 53                      magic       = 0x5753 ("WS", Big-Endian)
0002          01                         version     = 0x01 (1)
0003          08                         msg_type    = 0x08 (DATA)
0004 .. 0007  00 00 00 01                seq_num     = 0x00000001 (1, Big-Endian)
0008 .. 000B  00 00 22 74                payload_len = 0x00002274 (8820 bytes, Big-Endian)
000C .. 000F  00 00 00 00                param       = 0x00000000 (0, Big-Endian)
[Followed immediately by 8,820 bytes of raw stereo PCM audio at offsets 0010 .. 2283]
```

---

## 11. Planned for v1 Session 2 (Reserved Contract)

In Session 1, the server operates in unpaced burst mode and rejects `PAUSE`, `SEEK`, and `STOP` with `ERROR(INVALID_STATE=300)`. The wire format already reserves full support for Session 2 features without header modifications:

1. **Real-Time 50 ms Server Pacing (`clock_nanosleep`):**
   * In Session 2, the server paces `DATA` chunks using `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` at one chunk ($8,820\text{ bytes}$) every $50\text{ ms}$ ($20\text{ chunks/s}$) to match the real-time consumption rate of $176,400\text{ B/s}$ without cumulative timer drift.
2. **`PLAY` with Non-Zero Start Chunk (`param = start_chunk`):**
   * `param = 0` starts from the beginning (sending `seq = 0` WAV header followed by `seq = 1`).
   * `param = K` ($K \ge 1$) begins streaming directly from chunk $K$.
3. **`SEEK` (`0x06`):**
   * Client sends `SEEK` with `payload_len = 0` and `param = target_chunk` ($K \ge 1$).
   * Server computes the exact byte offset in the `.wav` file using:
     $$\text{file\_offset} = 44 + (\text{target\_chunk} - 1) \times 8,820$$
   * If `file_offset >= file_size` or `target_chunk == 0`, server replies `ERROR(SEEK_OUT_OF_RANGE=202)`. Otherwise, server calls `lseek(wav_fd, file_offset, SEEK_SET)` and resumes sending `DATA` frames with `seq_num = target_chunk`.
4. **`PAUSE` (`0x05`) and `STOP` (`0x07`):**
   * Sent by client with `payload_len = 0` and `param = 0` during an active stream.
   * `PAUSE` suspends chunk transmission until resumed; `STOP` aborts the current stream, sends `END` with total bytes transmitted so far, and returns the session to the idle command state.

---

## 12. Change Log

| Version | Date | Author | Summary of Changes |
| :--- | :--- | :--- | :--- |
| `v1.0` | Session 1 (Week 1) | Member 2 | Initial frozen protocol specification: 16-byte big-endian frame header, `0x01`–`0x0B` and `0xFF` opcodes, `0`–`500` error taxonomy, 8,820-byte chunk derivation, hex dumps, and Session 2 reservations. |