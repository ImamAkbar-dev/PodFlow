"""Katalog episode PodFlow untuk endpoint GET /api/catalog.

Owner: Imam (Anggota 2 - Inti Gateway: Protokol & Sesi)

Mengambil daftar nama file `.wav` langsung dari server C lewat frame `LIST` (`0x03`),
lalu melengkapi metadata (`size_bytes`, `duration_s`, `total_chunks`) dari ukuran
file / header WAV di direktori `media/` (1 chunk = 8820 byte = 50 ms, 20 chunk/s).

Format keluaran (Kontrak Tugas.md Bagian 2.3):
  {
    "files": [
      {
        "name": "sample_30s.wav",
        "size_bytes": 5292044,
        "duration_s": 30.0,
        "total_chunks": 600
      }
    ]
  }
"""

from __future__ import annotations

import asyncio
import contextlib
import math
from pathlib import Path
from typing import Any

try:
    from . import protocol
except ImportError:  # pragma: no cover
    import protocol  # type: ignore[no-redef]

from .protocol import (
    DEFAULT_PORT,
    MSG_ACK,
    MSG_ERROR,
    MSG_HELLO,
    MSG_LIST,
    MSG_WELCOME,
    PCM_CHUNK_SIZE,
    WAV_HEADER_SIZE,
    pack_frame,
    recv_frame,
)

CHUNKS_PER_SECOND = 20
BYTES_PER_SECOND = PCM_CHUNK_SIZE * CHUNKS_PER_SECOND  # 176,400 B/s


def resolve_media_dir(media_dir: str | Path | None = None) -> Path | None:
    """Locate the PodFlow `media/` directory relative to the repo or working dir."""
    if media_dir is not None:
        p = Path(media_dir)
        return p if p.is_dir() else None

    here = Path(__file__).resolve().parent
    candidates = [
        here.parent.parent / "media",  # repo_root/media
        Path.cwd() / "media",          # ./media
        Path.cwd().parent / "media",   # ../media (when cwd is ui/)
    ]
    for cand in candidates:
        if cand.is_dir():
            return cand
    return None


def compute_file_metadata(
    name: str,
    media_dir: str | Path | None = None,
) -> dict[str, Any]:
    """Build catalog metadata dict for a single `.wav` filename."""
    entry: dict[str, Any] = {
        "name": name,
        "size_bytes": None,
        "duration_s": None,
        "total_chunks": None,
    }

    # Prevent any path traversal when probing local media directory
    if not name or "/" in name or "\\" in name or ".." in name:
        return entry

    mdir = resolve_media_dir(media_dir)
    if mdir is None:
        return entry

    fpath = mdir / name
    try:
        st = fpath.stat()
    except OSError:
        return entry

    if st.st_size <= WAV_HEADER_SIZE:
        return entry

    size_bytes = int(st.st_size)
    pcm_bytes = size_bytes - WAV_HEADER_SIZE
    total_chunks = (pcm_bytes + PCM_CHUNK_SIZE - 1) // PCM_CHUNK_SIZE
    duration_s = round(pcm_bytes / float(BYTES_PER_SECOND), 3)

    entry["size_bytes"] = size_bytes
    entry["duration_s"] = duration_s
    entry["total_chunks"] = int(total_chunks)
    return entry


async def list_remote_files(
    server_host: str = "127.0.0.1",
    server_port: int = DEFAULT_PORT,
    *,
    timeout: float = 5.0,
) -> list[str]:
    """Query the C server using `HELLO` -> `WELCOME` and `LIST` -> `ACK`."""
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(server_host, int(server_port)),
        timeout=timeout,
    )
    try:
        # 1. Handshake HELLO -> WELCOME
        writer.write(pack_frame(MSG_HELLO, seq_num=0, param=0, payload="ui-catalog"))
        await asyncio.wait_for(writer.drain(), timeout=timeout)

        welcome = await asyncio.wait_for(recv_frame(reader), timeout=timeout)
        if welcome is None:
            raise ConnectionError("Server closed connection during handshake")
        if welcome.msg_type == MSG_ERROR:
            msg = welcome.payload.decode("utf-8", errors="replace")
            raise RuntimeError(f"Server error {welcome.param}: {msg}")
        if welcome.msg_type != MSG_WELCOME:
            raise RuntimeError(
                f"Expected WELCOME (0x02), got 0x{welcome.msg_type:02X}"
            )

        # 2. Request LIST -> ACK
        writer.write(pack_frame(MSG_LIST, seq_num=1, param=0, payload=b""))
        await asyncio.wait_for(writer.drain(), timeout=timeout)

        ack = await asyncio.wait_for(recv_frame(reader), timeout=timeout)
        if ack is None:
            raise ConnectionError("Server closed connection before LIST ACK")
        if ack.msg_type == MSG_ERROR:
            msg = ack.payload.decode("utf-8", errors="replace")
            raise RuntimeError(f"Server error {ack.param} on LIST: {msg}")
        if ack.msg_type != MSG_ACK:
            raise RuntimeError(f"Expected ACK (0x0A), got 0x{ack.msg_type:02X}")

        text = ack.payload.decode("utf-8", errors="replace")
        names = [line.strip() for line in text.splitlines() if line.strip()]
        return names
    finally:
        with contextlib.suppress(Exception):
            writer.close()
            await asyncio.wait_for(writer.wait_closed(), timeout=1.0)


async def fetch_catalog(
    server_host: str = "127.0.0.1",
    server_port: int = DEFAULT_PORT,
    media_dir: str | Path | None = None,
    *,
    timeout: float = 5.0,
) -> dict[str, list[dict[str, Any]]]:
    """Fetch the episode list from the C server and enrich with duration & chunk count."""
    names = await list_remote_files(
        server_host=server_host,
        server_port=server_port,
        timeout=timeout,
    )
    files = [compute_file_metadata(name, media_dir=media_dir) for name in names]
    return {"files": files}


# Convenience alias so gateway.py can import either fetch_catalog or get_catalog
get_catalog = fetch_catalog
