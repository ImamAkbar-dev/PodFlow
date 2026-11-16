# PodFlow — Concurrent Chunked Audio/Podcast Streaming Server

**PodFlow** is a high-performance, concurrent chunked audio streaming server and client suite written in C11 using POSIX sockets and `pthreads`. It streams uncompressed PCM WAV audio over TCP (Port 8888) using a custom 16-byte big-endian binary framing protocol with real-time 50 ms pacing, interactive playback controls (`PLAY`, `PAUSE`, `SEEK`, `STOP`), and a lock-aware ring buffer architecture.

---

## 🏗️ System Architecture & Protocol Specs (v1.1)

- **Language & Compiler**: C11 (`gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread -g`)
- **Transport**: TCP on Port `8888` (`TCP_NODELAY` enabled, `SO_SNDTIMEO = 5s`)
- **Framing**: 16-byte Big-Endian Header (`magic: 0x5753`, `version: 1`, `msg_type`, `seq_num`, `payload_len`, `param`)
- **Audio Payload**: Canonical 44-byte header WAV (PCM 16-bit, 44.1 kHz, Stereo). Each chunk is `8820` bytes (50 ms of audio).
- **Concurrency Model**:
  - **Connection Thread** (1 per client): Handles socket I/O via `ppoll()`, dispatches control commands, and enforces drift-free `CLOCK_MONOTONIC` 50 ms pacing.
  - **Producer Thread** (1 per session): Reads WAV chunks from disk into a 40-slot (2-second) `ring_buffer_t` with epoch-based flushing for instant `SEEK`.

---

## 📁 Repository Layout

```text
podflow/
├── Makefile                        # Build targets for server, client, and C unit tests
├── README.md                       # Project documentation and quickstart guide
├── .gitignore                      # Ignores binaries, WAVs, and PCAPs; tracks results/
├── Dockerfile                      # Optional containerized build environment
├── docker-compose.yml              # Container orchestration
├── include/                        # Shared header contracts
│   ├── protocol.h                  # Wire framing & error codes
│   ├── io_utils.h                  # EINTR-safe readn/writen
│   ├── pacing.h                    # CLOCK_MONOTONIC drift-free pacer
│   ├── ring_buffer.h               # Thread-safe bounded ring buffer with epoch
│   └── stream_session.h            # Playback state machine (IDLE/PLAYING/PAUSED)
├── src/
│   ├── common/                     # io_utils.c, protocol.c
│   ├── server/                     # server.c, pacing.c, ring_buffer.c, stream_session.c
│   └── client/                     # client.c (echo, play/get, scripted control, CSV logger)
├── media/                          # generate_wav.sh (creates sample_30s, sample_5m, sample_tail)
├── tests/                          # Automated bash & Python verification/benchmark suites
├── captures/                       # tcpdump & Wireshark inspection guides
├── results/                        # Benchmark CSVs and matplotlib PNG plots
└── docs/                           # Protocol spec, load-test/Wireshark report, demo script