"""Satu tab browser = satu koneksi TCP ke server C PodFlow.

Owner: Imam (Anggota 2 - Inti Gateway: Protokol & Sesi)

Tanggung jawab:
1. Satu koneksi WebSocket = satu koneksi TCP ke server C (`127.0.0.1:8888`).
2. Meneruskan frame biner 16-byte + payload dua arah apa adanya (tanpa pembungkus).
3. Menjaga backpressure: menghentikan pembacaan TCP (`pause_reading()`) saat buffer
   tulis WebSocket penuh/tinggi, dan melanjutkannya (`resume_reading()`) saat longgar.
4. Menerapkan aturan stale-DATA di sisi gateway: setelah mengirim `PAUSE`, `SEEK`,
   atau `STOP`, semua frame `DATA` yang masih in-flight diabaikan sampai `ACK`
   (atau `ERROR`) untuk perintah tersebut diterima dari server.
5. Meneruskan frame `ERROR` ke browser; menjaga sesi tetap terbuka untuk error
   recoverable (`102`, `200`, `201`, `202`, `300`) dan menutup WebSocket dengan
   kode khusus (`4100`, `4101`, `4103`, `4503`, `4900`, `4901`) untuk error fatal.
6. Membersihkan sumber daya saat tab ditutup (mengirim `STOP` bila sesi masih aktif
   memutar, lalu menutup soket TCP).
"""

from __future__ import annotations

import asyncio
import contextlib
import socket
import time
from enum import IntEnum
from typing import Any, Set

try:
    import aiohttp
    from aiohttp import web
    WSMsgType = aiohttp.WSMsgType
except ImportError:  # pragma: no cover - fallback when aiohttp is not installed
    aiohttp = None  # type: ignore[assignment]
    web = None  # type: ignore[assignment]

    class WSMsgType(IntEnum):  # type: ignore[no-redef]
        CONTINUATION = 0x0
        TEXT = 0x1
        BINARY = 0x2
        PING = 0x9
        PONG = 0xA
        CLOSE = 0x8
        CLOSING = 0x100
        CLOSED = 0x101
        ERROR = 0x102

try:
    from . import protocol
except ImportError:  # pragma: no cover - fallback when run as top-level script
    import protocol  # type: ignore[no-redef]

from .protocol import (
    DEFAULT_PORT,
    ERR_BAD_MAGIC,
    ERR_BAD_VERSION,
    ERR_PAYLOAD_TOO_LARGE,
    ERR_SERVER_BUSY,
    FRAME_HEADER_SIZE,
    MSG_ACK,
    MSG_DATA,
    MSG_END,
    MSG_ERROR,
    MSG_PAUSE,
    MSG_PLAY,
    MSG_SEEK,
    MSG_STOP,
    PROTO_MAGIC,
    PROTO_VERSION,
    ProtocolError,
    pack_frame,
    recv_frame,
    unpack_header,
)

# ============================================================================
# 1. WEBSOCKET CLOSE CODES (Tugas.md Section 2.2)
# ============================================================================

WS_CLOSE_BAD_MAGIC = 4100
WS_CLOSE_BAD_VERSION = 4101
WS_CLOSE_PAYLOAD_TOO_LARGE = 4103
WS_CLOSE_SERVER_BUSY = 4503
WS_CLOSE_CONNECT_FAILED = 4900
WS_CLOSE_SERVER_DISCONNECTED = 4901

FATAL_ERROR_CLOSE_CODES = {
    ERR_BAD_MAGIC: WS_CLOSE_BAD_MAGIC,
    ERR_BAD_VERSION: WS_CLOSE_BAD_VERSION,
    ERR_PAYLOAD_TOO_LARGE: WS_CLOSE_PAYLOAD_TOO_LARGE,
    ERR_SERVER_BUSY: WS_CLOSE_SERVER_BUSY,
}

DEFAULT_HIGH_WATERMARK = 65536  # 64 KiB (~7 PCM chunks)
DEFAULT_LOW_WATERMARK = 16384   # 16 KiB (~2 PCM chunks)

# ============================================================================
# 2. ACTIVE SESSION REGISTRY (Used by stats.py / gateway.py)
# ============================================================================

ACTIVE_SESSIONS: Set["Session"] = set()


def get_active_session_count() -> int:
    """Return the number of currently active WebSocket <-> TCP sessions."""
    return len(ACTIVE_SESSIONS)


def get_active_sessions() -> list["Session"]:
    """Return a snapshot list of active Session objects."""
    return list(ACTIVE_SESSIONS)


# ============================================================================
# 3. GATEWAY SESSION IMPLEMENTATION
# ============================================================================


class Session:
    """Manages a 1-to-1 bidirectional relay between a browser WebSocket and C server TCP socket."""

    def __init__(
        self,
        ws: web.WebSocketResponse,
        server_host: str = "127.0.0.1",
        server_port: int = DEFAULT_PORT,
        *,
        connect_timeout: float = 5.0,
        high_watermark: int = DEFAULT_HIGH_WATERMARK,
        low_watermark: int | None = None,
    ) -> None:
        self.ws = ws
        self.server_host = server_host
        self.server_port = int(server_port)
        self.connect_timeout = float(connect_timeout)
        self.high_watermark = max(1, int(high_watermark))
        self.low_watermark = (
            max(0, int(low_watermark))
            if low_watermark is not None
            else max(0, self.high_watermark // 4)
        )

        self.tcp_reader: asyncio.StreamReader | None = None
        self.tcp_writer: asyncio.StreamWriter | None = None

        # Protocol state & stale-DATA tracking
        self.stream_active: bool = False
        self.stop_requested: bool = False
        self.pending_control_acks: int = 0
        self.stale_chunks_dropped: int = 0

        # Backpressure & per-session telemetry
        self.tcp_paused: bool = False
        self.backpressure_events: int = 0
        self.bytes_relayed: int = 0
        self.chunks_relayed: int = 0
        self.frames_from_client: int = 0
        self.frames_from_server: int = 0
        self.last_chunk_mono_ns: int = 0
        self.jitter_ns: float = 0.0

        # Close disposition
        self.client_closed: bool = False
        self.close_code: int | None = None
        self.close_reason: str = ""
        self._closed: bool = False

    # ------------------------------------------------------------------
    # Context manager & connection lifecycle
    # ------------------------------------------------------------------

    async def __aenter__(self) -> "Session":
        await self.connect()
        return self

    async def __aexit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        await self.close()

    async def connect(self) -> bool:
        """Open the dedicated TCP connection to the C server.

        Returns True on success. On failure, sets `close_code = 4900`, closes
        the WebSocket with 4900, and returns False.
        """
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(
                    self.server_host,
                    self.server_port,
                    limit=self.high_watermark,
                ),
                timeout=self.connect_timeout,
            )
        except (OSError, asyncio.TimeoutError) as exc:
            self.close_code = WS_CLOSE_CONNECT_FAILED
            self.close_reason = f"Failed to connect to C server: {exc}"
            await self._close_ws(WS_CLOSE_CONNECT_FAILED, self.close_reason)
            return False

        sock = writer.get_extra_info("socket")
        if sock is not None:
            with contextlib.suppress(OSError):
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        self.tcp_reader = reader
        self.tcp_writer = writer
        return True

    async def run(self) -> None:
        """Run the bidirectional WebSocket <-> TCP relay until either side closes."""
        ACTIVE_SESSIONS.add(self)
        try:
            if self.tcp_reader is None or self.tcp_writer is None:
                connected = await self.connect()
                if not connected:
                    return

            ws_task = asyncio.create_task(self._ws_to_tcp(), name="podflow_ws_to_tcp")
            tcp_task = asyncio.create_task(self._tcp_to_ws(), name="podflow_tcp_to_ws")

            done, pending = await asyncio.wait(
                {ws_task, tcp_task},
                return_when=asyncio.FIRST_COMPLETED,
            )

            for task in pending:
                task.cancel()
            for task in pending:
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await task
            for task in done:
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    task.result()
        finally:
            await self.close()
            ACTIVE_SESSIONS.discard(self)

    # ------------------------------------------------------------------
    # Backpressure management (TCP pause/resume + WS transport drain)
    # ------------------------------------------------------------------

    def _get_ws_transport(self) -> asyncio.Transport | None:
        writer = getattr(self.ws, "_writer", None)
        if writer is not None and getattr(writer, "transport", None) is not None:
            return writer.transport
        req = getattr(self.ws, "_req", None)
        if req is not None and getattr(req, "transport", None) is not None:
            return req.transport
        return None

    def _get_ws_buffer_size(self) -> int:
        transport = self._get_ws_transport()
        if transport is not None and hasattr(transport, "get_write_buffer_size"):
            return int(transport.get_write_buffer_size())
        return 0

    def _pause_tcp_reading(self) -> None:
        if self.tcp_paused or self.tcp_writer is None:
            return
        transport = self.tcp_writer.transport
        if transport is not None and hasattr(transport, "pause_reading"):
            with contextlib.suppress(Exception):
                transport.pause_reading()
                self.tcp_paused = True
                self.backpressure_events += 1

    def _resume_tcp_reading(self) -> None:
        if not self.tcp_paused or self.tcp_writer is None:
            return
        transport = self.tcp_writer.transport
        if transport is not None and hasattr(transport, "resume_reading"):
            with contextlib.suppress(Exception):
                transport.resume_reading()
        self.tcp_paused = False

    async def _drain_ws_if_needed(self) -> None:
        """Pause TCP reading while WebSocket outbound buffer exceeds high watermark."""
        buf_size = self._get_ws_buffer_size()
        if buf_size < self.high_watermark:
            return

        self._pause_tcp_reading()
        try:
            writer = getattr(self.ws, "_writer", None)
            protocol_obj = getattr(writer, "protocol", None) if writer is not None else None
            drain_helper = getattr(protocol_obj, "_drain_helper", None)
            if callable(drain_helper):
                await drain_helper()
            else:
                # Fallback loop waiting for buffer to drop to low_watermark
                while (
                    not self.ws.closed
                    and self._get_ws_buffer_size() > self.low_watermark
                ):
                    await asyncio.sleep(0.005)
        finally:
            if self._get_ws_buffer_size() <= self.high_watermark:
                self._resume_tcp_reading()

    async def _send_ws_bytes(self, data: bytes) -> None:
        """Send a binary frame to the WebSocket while enforcing TCP backpressure."""
        if self.ws.closed:
            raise ConnectionResetError("WebSocket is already closed")

        await self._drain_ws_if_needed()

        # Also configure the underlying WS transport high-water mark so
        # ws.send_bytes() awaits _drain_helper() whenever the buffer fills.
        ws_transport = self._get_ws_transport()
        if ws_transport is not None and hasattr(ws_transport, "set_write_buffer_limits"):
            with contextlib.suppress(Exception):
                ws_transport.set_write_buffer_limits(
                    high=self.high_watermark,
                    low=self.low_watermark,
                )

        if self._get_ws_buffer_size() + len(data) >= self.high_watermark:
            self._pause_tcp_reading()

        try:
            await self.ws.send_bytes(data)
            await self._drain_ws_if_needed()
        finally:
            if self._get_ws_buffer_size() < self.high_watermark:
                self._resume_tcp_reading()

    # ------------------------------------------------------------------
    # Browser -> Server relay (_ws_to_tcp)
    # ------------------------------------------------------------------

    def _note_outbound_client_frame(self, raw: bytes) -> None:
        """Inspect client -> server frame header to track control commands & state."""
        if len(raw) < FRAME_HEADER_SIZE:
            return
        try:
            hdr = unpack_header(raw, validate=False)
        except ProtocolError:
            return

        if hdr.magic != PROTO_MAGIC or hdr.version != PROTO_VERSION:
            return

        if hdr.msg_type == MSG_PLAY:
            if hdr.payload_len > 0:
                self.stream_active = True
                self.stop_requested = False
        elif hdr.msg_type in (MSG_PAUSE, MSG_SEEK, MSG_STOP):
            self.pending_control_acks += 1
            if hdr.msg_type == MSG_STOP:
                self.stop_requested = True

    async def _ws_to_tcp(self) -> None:
        """Receive binary frames from the browser WebSocket and forward to TCP."""
        assert self.tcp_writer is not None

        async for msg in self.ws:
            if msg.type == WSMsgType.BINARY:
                raw = bytes(msg.data)
                if not raw:
                    continue
                self.frames_from_client += 1
                self._note_outbound_client_frame(raw)
                try:
                    self.tcp_writer.write(raw)
                    await self.tcp_writer.drain()
                except (OSError, ConnectionError) as exc:
                    if self.close_code is None:
                        self.close_code = WS_CLOSE_SERVER_DISCONNECTED
                        self.close_reason = f"TCP write failed: {exc}"
                    return
            elif msg.type in (
                WSMsgType.CLOSE,
                WSMsgType.CLOSING,
                WSMsgType.CLOSED,
            ):
                self.client_closed = True
                return
            elif msg.type == WSMsgType.ERROR:
                self.client_closed = True
                return
            # Text messages are ignored per Contract 2.1 ("Pesan teks tidak dipakai").

        self.client_closed = True

    # ------------------------------------------------------------------
    # Server -> Browser relay (_tcp_to_ws)
    # ------------------------------------------------------------------

    def _update_chunk_telemetry(self, payload_len: int, seq_num: int) -> None:
        """Update per-session byte/chunk counters and RFC 3550 jitter estimate."""
        self.bytes_relayed += payload_len
        if seq_num >= 1:
            self.chunks_relayed += 1
            now_ns = time.monotonic_ns()
            if self.last_chunk_mono_ns > 0:
                delta_ns = now_ns - self.last_chunk_mono_ns
                d_i = abs(delta_ns - 50_000_000)
                self.jitter_ns += (d_i - self.jitter_ns) / 16.0
            self.last_chunk_mono_ns = now_ns

    async def _tcp_to_ws(self) -> None:
        """Read PodFlow frames from TCP, filter stale DATA, and forward to WebSocket."""
        assert self.tcp_reader is not None

        while not self.ws.closed:
            try:
                frame = await recv_frame(self.tcp_reader)
            except ProtocolError as exc:
                self.close_code = exc.ws_close_code
                self.close_reason = str(exc)
                return
            except (OSError, ConnectionError) as exc:
                if self.close_code is None and not self.client_closed:
                    self.close_code = WS_CLOSE_SERVER_DISCONNECTED
                    self.close_reason = f"TCP read error: {exc}"
                return

            if frame is None:
                # Server closed TCP connection (EOF)
                if self.close_code is None and not self.client_closed:
                    self.close_code = WS_CLOSE_SERVER_DISCONNECTED
                    self.close_reason = "C server closed TCP connection"
                return

            self.frames_from_server += 1

            # 1. Stale-DATA Discard Rule (Tugas.md Section 2.5 & 3.1):
            #    After sending PAUSE, SEEK, or STOP, discard any in-flight DATA
            #    frames arriving before the server's ACK (or ERROR) response.
            if frame.msg_type == MSG_DATA and self.pending_control_acks > 0:
                self.stale_chunks_dropped += 1
                continue

            # 2. Control ACK / END state updates
            if frame.msg_type == MSG_ACK:
                if self.pending_control_acks > 0:
                    self.pending_control_acks -= 1
                if self.stop_requested and self.pending_control_acks == 0:
                    self.stream_active = False
                    self.stop_requested = False
            elif frame.msg_type == MSG_END:
                self.stream_active = False
                self.pending_control_acks = 0
                self.stop_requested = False
            elif frame.msg_type == MSG_ERROR:
                if self.pending_control_acks > 0:
                    self.pending_control_acks -= 1
                    self.stop_requested = False

                # Always forward the ERROR frame to the browser first
                with contextlib.suppress(Exception):
                    await self._send_ws_bytes(frame.raw)

                err_code = frame.param
                if err_code in FATAL_ERROR_CLOSE_CODES:
                    self.close_code = FATAL_ERROR_CLOSE_CODES[err_code]
                    self.close_reason = (
                        frame.payload.decode("utf-8", errors="replace")
                        or protocol.proto_err_name(err_code)
                    )
                    return
                # Recoverable error (102, 200, 201, 202, 300, etc.): keep session open
                continue
            elif frame.msg_type == MSG_DATA:
                self._update_chunk_telemetry(frame.payload_len, frame.seq_num)

            # 3. Forward binary frame to WebSocket with backpressure
            try:
                await self._send_ws_bytes(frame.raw)
            except (OSError, ConnectionError, RuntimeError):
                self.client_closed = True
                return

    # ------------------------------------------------------------------
    # Clean shutdown & resource release
    # ------------------------------------------------------------------

    async def _close_ws(self, code: int, reason: str = "") -> None:
        if self.ws.closed:
            return
        msg_bytes = reason.encode("utf-8", errors="ignore")[:120]
        with contextlib.suppress(Exception):
            await self.ws.close(code=code, message=msg_bytes)

    async def close(self) -> None:
        """Send STOP if tab closed mid-stream, close TCP socket, and close WebSocket."""
        if self._closed:
            return
        self._closed = True

        self._resume_tcp_reading()

        # If the browser closed the tab while a stream was still active, send a
        # best-effort STOP frame before closing the TCP socket (Contract 2.1).
        if (
            self.client_closed
            and self.stream_active
            and self.tcp_writer is not None
            and not self.tcp_writer.is_closing()
            and self.close_code not in FATAL_ERROR_CLOSE_CODES.values()
        ):
            with contextlib.suppress(Exception):
                self.tcp_writer.write(pack_frame(MSG_STOP, seq_num=0, param=0))
                await asyncio.wait_for(self.tcp_writer.drain(), timeout=0.5)
            self.stream_active = False

        if self.tcp_writer is not None:
            with contextlib.suppress(Exception):
                self.tcp_writer.close()
                await asyncio.wait_for(self.tcp_writer.wait_closed(), timeout=1.0)
            self.tcp_writer = None
            self.tcp_reader = None

        if not self.ws.closed:
            code = self.close_code if self.close_code is not None else 1000
            await self._close_ws(code, self.close_reason)


GatewaySession = Session


async def handle_websocket(
    ws: web.WebSocketResponse,
    server_host: str = "127.0.0.1",
    server_port: int = DEFAULT_PORT,
    **kwargs: Any,
) -> Session:
    """Convenience entrypoint to run a Session for an active aiohttp WebSocketResponse."""
    session = Session(ws, server_host=server_host, server_port=server_port, **kwargs)
    await session.run()
    return session
