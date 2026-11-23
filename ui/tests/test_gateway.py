"""End-to-end WebSocket <-> Gateway Session <-> Server C tests.

Owner: Imam (Anggota 2 - Inti Gateway: Protokol & Sesi)

Menguji seluruh kriteria selesai Anggota 2 (Tugas.md Bagian 3.1):
1. HELLO -> WELCOME, ECHO, LIST, dan catalog.fetch_catalog() lewat gateway ke server nyata.
2. PLAY sampai END lewat WebSocket dengan total byte cocok (`END.param` == total byte DATA termasuk 44B WAV header).
3. PAUSE, RESUME, SEEK, STOP lewat WebSocket tanpa menghasilkan DATA basi (stale-DATA discard rule).
4. Klien lambat tetap memicu backpressure (`tcp_paused` / `backpressure_events`) ke server.
5. Error recoverable (200, 202, 300) menjaga koneksi tetap terbuka, sedangkan 503 (`SERVER_BUSY`) menutup WebSocket dengan kode 4503 (dan 100/101/103 -> 4100/4101/4103, gagal konek -> 4900).
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import socket
import struct
import subprocess
import sys
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Any

UI_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = UI_DIR.parent
if str(UI_DIR) not in sys.path:
    sys.path.insert(0, str(UI_DIR))

from gateway.catalog import fetch_catalog  # noqa: E402
from gateway.protocol import (  # noqa: E402
    ERR_FILE_NOT_FOUND,
    ERR_INVALID_STATE,
    ERR_SEEK_OUT_OF_RANGE,
    ERR_SERVER_BUSY,
    HEADER_STRUCT,
    MSG_ACK,
    MSG_DATA,
    MSG_ECHO,
    MSG_END,
    MSG_ERROR,
    MSG_HELLO,
    MSG_LIST,
    MSG_PAUSE,
    MSG_PLAY,
    MSG_SEEK,
    MSG_STOP,
    MSG_WELCOME,
    PCM_CHUNK_SIZE,
    PROTO_VERSION,
    WAV_HEADER_SIZE,
    pack_frame,
    recv_frame,
    unpack_frame,
)
from gateway.session import (  # noqa: E402
    Session,
    WS_CLOSE_BAD_MAGIC,
    WS_CLOSE_CONNECT_FAILED,
    WS_CLOSE_SERVER_BUSY,
    WSMsgType,
    get_active_session_count,
)


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _write_canonical_wav(path: Path, pcm_bytes: int) -> None:
    """Write a canonical 44-byte RIFF/WAVE 16-bit 44.1 kHz stereo PCM file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    riff_size = 36 + pcm_bytes
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        riff_size,
        b"WAVE",
        b"fmt ",
        16,         # Subchunk1Size (PCM)
        1,          # AudioFormat (1 = PCM)
        2,          # NumChannels (2 = Stereo)
        44100,      # SampleRate
        176400,     # ByteRate (44100 * 2 * 2)
        4,          # BlockAlign (2 * 2)
        16,         # BitsPerSample
        b"data",
        pcm_bytes,
    )
    assert len(header) == WAV_HEADER_SIZE
    # Deterministic non-zero stereo PCM pattern
    block = bytes(((i * 37 + 13) & 0xFF) for i in range(PCM_CHUNK_SIZE))
    with path.open("wb") as fp:
        fp.write(header)
        rem = pcm_bytes
        while rem > 0:
            n = min(rem, len(block))
            fp.write(block[:n])
            rem -= n


def _ensure_server_and_media() -> Path:
    """Ensure the C `./server` binary and `media/*.wav` test files exist."""
    server_bin = REPO_ROOT / "server"
    if not server_bin.is_file() or not os.access(server_bin, os.X_OK):
        subprocess.run(
            ["make", "server"],
            cwd=str(REPO_ROOT),
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    sample_30s = REPO_ROOT / "media" / "sample_30s.wav"
    sample_tail = REPO_ROOT / "media" / "sample_tail.wav"
    if not sample_30s.is_file() or sample_30s.stat().st_size != 5292044:
        _write_canonical_wav(sample_30s, 600 * PCM_CHUNK_SIZE)
    if not sample_tail.is_file() or sample_tail.stat().st_size != 418112:
        _write_canonical_wav(sample_tail, 47 * PCM_CHUNK_SIZE + 3528)

    return server_bin


async def _wait_tcp_port(host: str, port: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
            return
        except OSError:
            await asyncio.sleep(0.03)
    raise TimeoutError(f"Server on {host}:{port} did not become ready within {timeout}s")


# ============================================================================
# Lightweight aiohttp-compatible WebSocket Endpoint Pair for Testing
# ============================================================================


@dataclass
class WSMessage:
    type: int
    data: Any = None
    extra: str = ""


class _SimulatedWSTransport:
    """Simulates aiohttp's underlying WebSocket write buffer for backpressure tests."""

    def __init__(self) -> None:
        self.buffered_bytes: int = 0
        self.high: int = 65536
        self.low: int = 16384
        self._drained_evt = asyncio.Event()
        self._drained_evt.set()

    def get_write_buffer_size(self) -> int:
        return self.buffered_bytes

    def set_write_buffer_limits(self, high: int = 65536, low: int = 16384) -> None:
        self.high = high
        self.low = low
        self._check_state()

    def add_bytes(self, n: int) -> None:
        self.buffered_bytes += n
        self._check_state()

    def consume_bytes(self, n: int) -> None:
        self.buffered_bytes = max(0, self.buffered_bytes - n)
        self._check_state()

    def force_release(self) -> None:
        self.buffered_bytes = 0
        self._drained_evt.set()

    def _check_state(self) -> None:
        if self.buffered_bytes >= self.high:
            self._drained_evt.clear()
        elif self.buffered_bytes <= self.low:
            self._drained_evt.set()

    async def wait_drained(self) -> None:
        await self._drained_evt.wait()


class _SimulatedWSProtocol:
    def __init__(self, transport: _SimulatedWSTransport) -> None:
        self._transport = transport

    async def _drain_helper(self) -> None:
        await self._transport.wait_drained()


class _SimulatedWSWriter:
    def __init__(self, transport: _SimulatedWSTransport) -> None:
        self.transport = transport
        self.protocol = _SimulatedWSProtocol(transport)


class ServerSideWebSocket:
    """Drop-in compatible with `aiohttp.web.WebSocketResponse`."""

    def __init__(
        self,
        in_q: asyncio.Queue[WSMessage],
        out_q: asyncio.Queue[WSMessage],
        transport: _SimulatedWSTransport,
    ) -> None:
        self._in_q = in_q
        self._out_q = out_q
        self._transport = transport
        self._writer = _SimulatedWSWriter(transport)
        self.closed: bool = False
        self.close_code: int | None = None
        self.close_message: bytes = b""

    def __aiter__(self) -> "ServerSideWebSocket":
        return self

    async def __anext__(self) -> WSMessage:
        if self.closed:
            raise StopAsyncIteration
        msg = await self._in_q.get()
        if msg.type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED):
            self.closed = True
            if self.close_code is None and isinstance(msg.data, int):
                self.close_code = msg.data
            return msg
        return msg

    async def send_bytes(self, data: bytes) -> None:
        if self.closed:
            raise ConnectionResetError("WebSocket is closed")
        raw = bytes(data)
        self._transport.add_bytes(len(raw))
        await self._out_q.put(WSMessage(type=WSMsgType.BINARY, data=raw))

    async def close(self, *, code: int = 1000, message: bytes = b"") -> bool:
        if self.closed:
            return False
        self.closed = True
        self.close_code = code
        self.close_message = bytes(message)
        self._transport.force_release()
        await self._out_q.put(
            WSMessage(
                type=WSMsgType.CLOSE,
                data=code,
                extra=self.close_message.decode("utf-8", errors="replace"),
            )
        )
        return True


class ClientSideWebSocket:
    """Drop-in compatible with `aiohttp.ClientWebSocketResponse`."""

    def __init__(
        self,
        to_server_q: asyncio.Queue[WSMessage],
        from_server_q: asyncio.Queue[WSMessage],
        transport: _SimulatedWSTransport,
        session: Session,
    ) -> None:
        self._to_server_q = to_server_q
        self._from_server_q = from_server_q
        self._transport = transport
        self.session = session
        self._task: asyncio.Task[None] | None = None
        self.closed: bool = False
        self.close_code: int | None = None

    async def __aenter__(self) -> "ClientSideWebSocket":
        self._task = asyncio.create_task(self.session.run())
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        await self.close()
        if self._task is not None:
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await asyncio.wait_for(self._task, timeout=2.0)

    async def send_bytes(self, data: bytes) -> None:
        if self.closed:
            raise ConnectionResetError("Client WebSocket is closed")
        await self._to_server_q.put(WSMessage(type=WSMsgType.BINARY, data=bytes(data)))

    async def receive(self, timeout: float = 10.0) -> WSMessage:
        msg = await asyncio.wait_for(self._from_server_q.get(), timeout=timeout)
        if msg.type == WSMsgType.BINARY:
            self._transport.consume_bytes(len(msg.data))
        elif msg.type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED):
            self.closed = True
            self.close_code = int(msg.data) if msg.data is not None else 1000
        return msg

    async def receive_bytes(self, timeout: float = 10.0) -> bytes:
        msg = await self.receive(timeout=timeout)
        if msg.type != WSMsgType.BINARY:
            raise TypeError(
                f"Expected BINARY WS message, got type={msg.type} (close_code={self.close_code})"
            )
        return bytes(msg.data)

    async def close(self, code: int = 1000) -> None:
        if not self.closed:
            self.closed = True
            self.close_code = code
            self._transport.force_release()
            await self._to_server_q.put(WSMessage(type=WSMsgType.CLOSE, data=code))


def _connect_ws_session(
    server_host: str,
    server_port: int,
    *,
    high_watermark: int = 65536,
) -> ClientSideWebSocket:
    """Create a paired ClientSideWebSocket <-> Session connected to (server_host, server_port)."""
    to_server_q: asyncio.Queue[WSMessage] = asyncio.Queue()
    from_server_q: asyncio.Queue[WSMessage] = asyncio.Queue()
    transport = _SimulatedWSTransport()
    transport.set_write_buffer_limits(high=high_watermark, low=max(0, high_watermark // 4))
    server_ws = ServerSideWebSocket(to_server_q, from_server_q, transport)
    sess = Session(
        server_ws,  # type: ignore[arg-type]
        server_host=server_host,
        server_port=server_port,
        high_watermark=high_watermark,
    )
    return ClientSideWebSocket(to_server_q, from_server_q, transport, sess)


class GatewayTest(unittest.TestCase):
    """End-to-end tests of gateway/protocol.py, gateway/session.py, and gateway/catalog.py."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.server_bin = _ensure_server_and_media()

    def _spawn_c_server(
        self,
        port: int,
        *,
        no_pacing: bool = True,
        max_clients: int | None = None,
    ) -> subprocess.Popen[bytes]:
        cmd = [str(self.server_bin), str(port)]
        if no_pacing:
            cmd.append("--no-pacing")
        if max_clients is not None:
            cmd.extend(["--max-clients", str(max_clients)])
        return subprocess.Popen(
            cmd,
            cwd=str(REPO_ROOT),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _stop_c_server(self, proc: subprocess.Popen[bytes]) -> None:
        if proc.poll() is not None:
            return
        proc.terminate()
        try:
            proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2.0)

    def test_1_handshake_echo_list_and_catalog(self) -> None:
        """HELLO -> WELCOME, ECHO, LIST, and catalog.fetch_catalog() against the real C server."""

        async def scenario() -> None:
            c_port = _find_free_port()
            proc = self._spawn_c_server(c_port, no_pacing=True)
            try:
                await _wait_tcp_port("127.0.0.1", c_port)

                # 1. Test catalog.fetch_catalog -> LIST
                cat = await fetch_catalog(
                    server_host="127.0.0.1",
                    server_port=c_port,
                    media_dir=REPO_ROOT / "media",
                )
                self.assertIn("files", cat)
                by_name = {f["name"]: f for f in cat["files"]}
                self.assertIn("sample_30s.wav", by_name)
                s30 = by_name["sample_30s.wav"]
                self.assertEqual(s30["size_bytes"], 5292044)
                self.assertEqual(s30["duration_s"], 30.0)
                self.assertEqual(s30["total_chunks"], 600)

                # 2. Test WebSocket HELLO -> WELCOME, ECHO, LIST -> ACK
                async with _connect_ws_session("127.0.0.1", c_port) as ws:
                    await ws.send_bytes(
                        pack_frame(MSG_HELLO, seq_num=0, param=0, payload="web-tab-1")
                    )
                    welcome = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(welcome.msg_type, MSG_WELCOME)
                    self.assertEqual(welcome.param, PCM_CHUNK_SIZE)
                    self.assertEqual(welcome.payload.decode("utf-8"), "podflow/1")
                    self.assertEqual(get_active_session_count(), 1)

                    # ECHO
                    echo_payload = b"PodFlow-UI-Gateway-Echo-Test-Payload" * 20
                    await ws.send_bytes(
                        pack_frame(MSG_ECHO, seq_num=7, param=12345, payload=echo_payload)
                    )
                    echo_reply = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(echo_reply.msg_type, MSG_ECHO)
                    self.assertEqual(echo_reply.seq_num, 7)
                    self.assertEqual(echo_reply.param, 12345)
                    self.assertEqual(echo_reply.payload, echo_payload)

                    # LIST -> ACK
                    await ws.send_bytes(pack_frame(MSG_LIST, seq_num=8, param=0))
                    list_ack = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(list_ack.msg_type, MSG_ACK)
                    self.assertEqual(list_ack.seq_num, 8)
                    files = list_ack.payload.decode("utf-8").splitlines()
                    self.assertIn("sample_30s.wav", files)
                    self.assertIn("sample_tail.wav", files)
            finally:
                self._stop_c_server(proc)

        asyncio.run(scenario())

    def test_2_play_end_bytes_match(self) -> None:
        """PLAY over WebSocket through gateway to END; verify total bytes == END.param."""

        async def scenario() -> None:
            c_port = _find_free_port()
            proc = self._spawn_c_server(c_port, no_pacing=True)
            try:
                await _wait_tcp_port("127.0.0.1", c_port)

                async with _connect_ws_session("127.0.0.1", c_port) as ws:
                    await ws.send_bytes(
                        pack_frame(MSG_HELLO, seq_num=0, param=0, payload="m2-play")
                    )
                    _ = unpack_frame(await ws.receive_bytes())

                    # Play sample_30s.wav (600 chunks = 5,292,000 B + 44 B hdr = 5,292,044 B)
                    await ws.send_bytes(
                        pack_frame(MSG_PLAY, seq_num=1, param=0, payload="sample_30s.wav")
                    )

                    ack = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack.msg_type, MSG_ACK)
                    self.assertEqual(ack.seq_num, 1)
                    self.assertEqual(ack.param, 1)

                    total_bytes = 0
                    expected_seq = 0
                    pcm_chunks = 0

                    while True:
                        frame = unpack_frame(await ws.receive_bytes())
                        if frame.msg_type == MSG_DATA:
                            self.assertEqual(frame.seq_num, expected_seq)
                            if expected_seq == 0:
                                self.assertEqual(frame.payload_len, WAV_HEADER_SIZE)
                                self.assertEqual(frame.payload[:4], b"RIFF")
                            else:
                                self.assertLessEqual(frame.payload_len, PCM_CHUNK_SIZE)
                                pcm_chunks += 1
                            total_bytes += frame.payload_len
                            expected_seq += 1
                        elif frame.msg_type == MSG_END:
                            self.assertEqual(frame.seq_num, pcm_chunks + 1)
                            self.assertEqual(frame.param, total_bytes)
                            self.assertEqual(total_bytes, 5292044)
                            self.assertEqual(pcm_chunks, 600)
                            break
                        else:
                            self.fail(f"Unexpected frame during PLAY: {frame}")
            finally:
                self._stop_c_server(proc)

        asyncio.run(scenario())

    def test_3_pause_resume_seek_stop_no_stale_data(self) -> None:
        """Verify PAUSE, RESUME, SEEK, STOP over WebSocket and stale-DATA discard rule."""

        async def scenario() -> None:
            # Part A: Test against real C server (with 50 ms pacing enabled so we can
            # interactively PAUSE, RESUME, SEEK, and STOP mid-stream).
            c_port = _find_free_port()
            proc = self._spawn_c_server(c_port, no_pacing=False)
            try:
                await _wait_tcp_port("127.0.0.1", c_port)

                async with _connect_ws_session("127.0.0.1", c_port) as ws:
                    await ws.send_bytes(pack_frame(MSG_HELLO, seq_num=0, param=0, payload="ctrl"))
                    _ = unpack_frame(await ws.receive_bytes())

                    # Start PLAY sample_30s.wav
                    await ws.send_bytes(
                        pack_frame(MSG_PLAY, seq_num=1, param=1, payload="sample_30s.wav")
                    )
                    ack_play = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack_play.msg_type, MSG_ACK)
                    self.assertEqual(ack_play.param, 1)

                    # Read WAV header (seq=0) and chunk 1 (seq=1)
                    hdr_frame = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(hdr_frame.msg_type, MSG_DATA)
                    self.assertEqual(hdr_frame.seq_num, 0)
                    c1_frame = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(c1_frame.msg_type, MSG_DATA)
                    self.assertEqual(c1_frame.seq_num, 1)

                    # 1. Send PAUSE -> must receive ACK without any stale DATA
                    await ws.send_bytes(pack_frame(MSG_PAUSE, seq_num=2, param=0))
                    ack_pause = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack_pause.msg_type, MSG_ACK)
                    self.assertEqual(ack_pause.seq_num, 2)
                    paused_next_seq = ack_pause.param
                    self.assertGreaterEqual(paused_next_seq, 2)

                    # 2. Send PLAY("") (Resume) -> ACK(paused_next_seq) then DATA(paused_next_seq)
                    await ws.send_bytes(pack_frame(MSG_PLAY, seq_num=3, param=0, payload=b""))
                    ack_res = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack_res.msg_type, MSG_ACK)
                    self.assertEqual(ack_res.seq_num, 3)
                    self.assertEqual(ack_res.param, paused_next_seq)

                    resumed_data = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(resumed_data.msg_type, MSG_DATA)
                    self.assertEqual(resumed_data.seq_num, paused_next_seq)

                    # 3. Send SEEK(target=250) -> must NOT receive any stale DATA prior to ACK(250),
                    #    and first DATA after ACK(250) must have seq_num == 250 (no WAV header resent).
                    await ws.send_bytes(pack_frame(MSG_SEEK, seq_num=4, param=250))
                    ack_seek = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack_seek.msg_type, MSG_ACK)
                    self.assertEqual(ack_seek.seq_num, 4)
                    self.assertEqual(ack_seek.param, 250)

                    seek_data = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(seek_data.msg_type, MSG_DATA)
                    self.assertEqual(seek_data.seq_num, 250)
                    self.assertEqual(seek_data.payload_len, PCM_CHUNK_SIZE)

                    # 4. Send STOP -> must receive ACK(last_sent_seq), NO END frame, and return to IDLE
                    await ws.send_bytes(pack_frame(MSG_STOP, seq_num=5, param=0))
                    ack_stop = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack_stop.msg_type, MSG_ACK)
                    self.assertEqual(ack_stop.seq_num, 5)
                    self.assertGreaterEqual(ack_stop.param, 250)

                    # Verify connection returned to IDLE and ECHO works immediately without stray END/DATA
                    await ws.send_bytes(pack_frame(MSG_ECHO, seq_num=6, param=99, payload="after-stop"))
                    echo_after = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(echo_after.msg_type, MSG_ECHO)
                    self.assertEqual(echo_after.seq_num, 6)
                    self.assertEqual(echo_after.payload, b"after-stop")
            finally:
                self._stop_c_server(proc)

            # Part B: Deterministic in-flight stale-DATA race test on Session.
            # Simulates a TCP server that already had DATA(seq=11) and DATA(seq=12)
            # in flight on the wire when the client transmitted SEEK(target=100).
            async def mock_tcp_server(
                reader: asyncio.StreamReader, writer: asyncio.StreamWriter
            ) -> None:
                try:
                    cmd = await recv_frame(reader)
                    assert cmd is not None and cmd.msg_type == MSG_SEEK
                    writer.write(pack_frame(MSG_DATA, seq_num=11, param=0, payload=b"STALE_11"))
                    writer.write(pack_frame(MSG_DATA, seq_num=12, param=0, payload=b"STALE_12"))
                    writer.write(pack_frame(MSG_ACK, seq_num=cmd.seq_num, param=100))
                    writer.write(pack_frame(MSG_DATA, seq_num=100, param=0, payload=b"FRESH_100"))
                    await writer.drain()
                    await reader.read(16)
                finally:
                    writer.close()

            srv = await asyncio.start_server(mock_tcp_server, "127.0.0.1", 0)
            mock_port = srv.sockets[0].getsockname()[1]
            try:
                async with _connect_ws_session("127.0.0.1", mock_port) as ws:
                    await ws.send_bytes(pack_frame(MSG_SEEK, seq_num=4, param=100))
                    first = unpack_frame(await ws.receive_bytes())
                    second = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(first.msg_type, MSG_ACK)
                    self.assertEqual(first.param, 100)
                    self.assertEqual(second.msg_type, MSG_DATA)
                    self.assertEqual(second.seq_num, 100)
                    self.assertEqual(second.payload, b"FRESH_100")
                    self.assertEqual(ws.session.stale_chunks_dropped, 2)
            finally:
                srv.close()
                await srv.wait_closed()

        asyncio.run(scenario())

    def test_4_error_codes_and_503_close_code(self) -> None:
        """Verify recoverable errors (200, 202, 300), 503 -> 4503, 100 -> 4100, and 4900."""

        async def scenario() -> None:
            # 1. Unreachable C server -> WebSocket closes with 4900
            unused_port = _find_free_port()
            async with _connect_ws_session("127.0.0.1", unused_port) as ws_dead:
                msg = await ws_dead.receive()
                self.assertIn(
                    msg.type,
                    (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED),
                )
                self.assertEqual(ws_dead.close_code, WS_CLOSE_CONNECT_FAILED)

            # 2. Real C server with --max-clients 1:
            #    - Client 1 tests recoverable errors (200, 202, 300) and stays connected
            #    - Client 2 gets ERROR(503 SERVER_BUSY) and WebSocket closes with 4503
            #    - Client 1 sends bad magic (0xDEAD) and gets ERROR(100) + close 4100
            c_port = _find_free_port()
            proc = self._spawn_c_server(c_port, no_pacing=True, max_clients=1)
            try:
                await _wait_tcp_port("127.0.0.1", c_port)

                async with _connect_ws_session("127.0.0.1", c_port) as ws1:
                    await ws1.send_bytes(pack_frame(MSG_HELLO, seq_num=0, param=0, payload="c1"))
                    w1 = unpack_frame(await ws1.receive_bytes())
                    self.assertEqual(w1.msg_type, MSG_WELCOME)

                    # Recoverable Error 300: PAUSE while IDLE -> session stays open
                    await ws1.send_bytes(pack_frame(MSG_PAUSE, seq_num=10, param=0))
                    err300 = unpack_frame(await ws1.receive_bytes())
                    self.assertEqual(err300.msg_type, MSG_ERROR)
                    self.assertEqual(err300.param, ERR_INVALID_STATE)

                    # Recoverable Error 200: PLAY missing file -> session stays open
                    await ws1.send_bytes(
                        pack_frame(MSG_PLAY, seq_num=11, param=0, payload="nonexistent.wav")
                    )
                    err200 = unpack_frame(await ws1.receive_bytes())
                    self.assertEqual(err200.msg_type, MSG_ERROR)
                    self.assertEqual(err200.param, ERR_FILE_NOT_FOUND)

                    # Recoverable Error 202: PLAY with start_chunk=99999 -> session stays open
                    await ws1.send_bytes(
                        pack_frame(
                            MSG_PLAY,
                            seq_num=12,
                            param=99999,
                            payload="sample_tail.wav",
                        )
                    )
                    err202 = unpack_frame(await ws1.receive_bytes())
                    self.assertEqual(err202.msg_type, MSG_ERROR)
                    self.assertEqual(err202.param, ERR_SEEK_OUT_OF_RANGE)

                    # Open second tab (ws2) while max-clients=1 is full -> 503 / WS close 4503
                    async with _connect_ws_session("127.0.0.1", c_port) as ws2:
                        first_msg = await ws2.receive()
                        if first_msg.type == WSMsgType.BINARY:
                            err503 = unpack_frame(first_msg.data)
                            self.assertEqual(err503.msg_type, MSG_ERROR)
                            self.assertEqual(err503.param, ERR_SERVER_BUSY)
                            close_msg = await ws2.receive()
                            self.assertIn(
                                close_msg.type,
                                (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED),
                            )
                        self.assertEqual(ws2.close_code, WS_CLOSE_SERVER_BUSY)

                    # Fatal framing error on ws1: send magic=0xDEAD -> ERROR(100) + close 4100
                    bad_frame = HEADER_STRUCT.pack(0xDEAD, PROTO_VERSION, MSG_HELLO, 99, 0, 0)
                    await ws1.send_bytes(bad_frame)
                    m1 = await ws1.receive()
                    if m1.type == WSMsgType.BINARY:
                        err100 = unpack_frame(m1.data)
                        self.assertEqual(err100.msg_type, MSG_ERROR)
                        self.assertEqual(err100.param, 100)
                        await ws1.receive()
                    self.assertEqual(ws1.close_code, WS_CLOSE_BAD_MAGIC)
            finally:
                self._stop_c_server(proc)

        asyncio.run(scenario())

    def test_5_slow_client_backpressure(self) -> None:
        """Verify that a slow WebSocket consumer triggers TCP backpressure in Session."""

        async def scenario() -> None:
            c_port = _find_free_port()
            proc = self._spawn_c_server(c_port, no_pacing=True)
            try:
                await _wait_tcp_port("127.0.0.1", c_port)
                # Configure a small high_watermark (4 KiB) so backpressure engages
                # as soon as the WebSocket client stops reading PCM chunks.
                async with _connect_ws_session(
                    "127.0.0.1",
                    c_port,
                    high_watermark=4096,
                ) as ws:
                    await ws.send_bytes(
                        pack_frame(MSG_PLAY, seq_num=1, param=0, payload="sample_30s.wav")
                    )
                    # Read only the ACK frame, then stop reading from WebSocket
                    ack = unpack_frame(await ws.receive_bytes())
                    self.assertEqual(ack.msg_type, MSG_ACK)

                    # Wait briefly while the server pumps 8,820-byte chunks (> 4096 watermark)
                    deadline = time.monotonic() + 2.0
                    while time.monotonic() < deadline:
                        if ws.session.backpressure_events > 0 and ws.session.tcp_paused:
                            break
                        await asyncio.sleep(0.02)

                    self.assertGreater(
                        ws.session.backpressure_events,
                        0,
                        "Expected gateway Session to pause TCP reading on slow WebSocket client",
                    )
                    self.assertTrue(ws.session.tcp_paused)
            finally:
                self._stop_c_server(proc)

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
