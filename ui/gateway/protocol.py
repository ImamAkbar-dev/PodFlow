"""Pack/unpack frame PodFlow v1.1 (cermin dari include/protocol.h).

Owner: Imam (Anggota 2 - Inti Gateway: Protokol & Sesi)
Header 16 byte, big-endian (Network Byte Order):
  Offset  Size  Type      Field
  0       2     uint16_t  magic       (0x5753 = "WS")
  2       1     uint8_t   version     (1)
  3       1     uint8_t   msg_type    (0x01..0x0B, 0xFF)
  4       4     uint32_t  seq_num
  8       4     uint32_t  payload_len (0..65536)
  12      4     uint32_t  param
"""

from __future__ import annotations

import asyncio
import struct
from typing import NamedTuple, Union

# ============================================================================
# 1. PROTOCOL CONSTANTS (Mirrored from include/protocol.h)
# ============================================================================

PROTO_MAGIC = 0x5753
PROTO_VERSION = 1
DEFAULT_PORT = 8888

WAV_HEADER_SIZE = 44
PCM_CHUNK_SIZE = 8820
MAX_PAYLOAD_SIZE = 65536
FRAME_HEADER_SIZE = 16

WELCOME_BANNER = "podflow/1"

# ============================================================================
# 2. MESSAGE TYPES (u8 on wire)
# ============================================================================

HELLO = 0x01
WELCOME = 0x02
LIST = 0x03
PLAY = 0x04
PAUSE = 0x05
SEEK = 0x06
STOP = 0x07
DATA = 0x08
END = 0x09
ACK = 0x0A
ECHO = 0x0B
ERROR = 0xFF

MSG_HELLO = 0x01
MSG_WELCOME = 0x02
MSG_LIST = 0x03
MSG_PLAY = 0x04
MSG_PAUSE = 0x05
MSG_SEEK = 0x06
MSG_STOP = 0x07
MSG_DATA = 0x08
MSG_END = 0x09
MSG_ACK = 0x0A
MSG_ECHO = 0x0B
MSG_ERROR = 0xFF

MSG = {
    "HELLO": MSG_HELLO,
    "WELCOME": MSG_WELCOME,
    "LIST": MSG_LIST,
    "PLAY": MSG_PLAY,
    "PAUSE": MSG_PAUSE,
    "SEEK": MSG_SEEK,
    "STOP": MSG_STOP,
    "DATA": MSG_DATA,
    "END": MSG_END,
    "ACK": MSG_ACK,
    "ECHO": MSG_ECHO,
    "ERROR": MSG_ERROR,
}

# ============================================================================
# 3. ERROR CODES (carried in frame_hdr_t.param of ERROR frames, u32)
# ============================================================================

NONE = 0
BAD_MAGIC = 100
BAD_VERSION = 101
UNKNOWN_CMD = 102
PAYLOAD_TOO_LARGE = 103
FILE_NOT_FOUND = 200
INVALID_WAV = 201
SEEK_OUT_OF_RANGE = 202
INVALID_STATE = 300
INTERNAL_SERVER = 500
SERVER_BUSY = 503

ERR_NONE = 0
ERR_BAD_MAGIC = 100
ERR_BAD_VERSION = 101
ERR_UNKNOWN_CMD = 102
ERR_PAYLOAD_TOO_LARGE = 103
ERR_FILE_NOT_FOUND = 200
ERR_INVALID_WAV = 201
ERR_SEEK_OUT_OF_RANGE = 202
ERR_INVALID_STATE = 300
ERR_INTERNAL_SERVER = 500
ERR_SERVER_BUSY = 503

ERR = {
    "NONE": ERR_NONE,
    "BAD_MAGIC": ERR_BAD_MAGIC,
    "BAD_VERSION": ERR_BAD_VERSION,
    "UNKNOWN_CMD": ERR_UNKNOWN_CMD,
    "PAYLOAD_TOO_LARGE": ERR_PAYLOAD_TOO_LARGE,
    "FILE_NOT_FOUND": ERR_FILE_NOT_FOUND,
    "INVALID_WAV": ERR_INVALID_WAV,
    "SEEK_OUT_OF_RANGE": ERR_SEEK_OUT_OF_RANGE,
    "INVALID_STATE": ERR_INVALID_STATE,
    "INTERNAL_SERVER": ERR_INTERNAL_SERVER,
    "SERVER_BUSY": ERR_SERVER_BUSY,
}

# ============================================================================
# 4. RETURN CODES FOR recv_frame() (C Parity)
# ============================================================================

RECV_OK = 1
RECV_EOF = 0
RECV_ERR_IO = -1
RECV_ERR_BAD_MAGIC = -2
RECV_ERR_BAD_VERSION = -3
RECV_ERR_PAYLOAD_LEN = -4

# ============================================================================
# 5. WIRE HEADER STRUCT & FRAME REPRESENTATION
# ============================================================================

HEADER_FORMAT = "!HBBIII"
HEADER_STRUCT = struct.Struct(HEADER_FORMAT)
assert HEADER_STRUCT.size == FRAME_HEADER_SIZE


class FrameHeader(NamedTuple):
    """Unpacked 16-byte PodFlow wire header in host order."""

    magic: int
    version: int
    msg_type: int
    seq_num: int
    payload_len: int
    param: int


class Frame:
    """Complete PodFlow wire frame (16-byte header + payload).

    Supports both attribute access (`frame.msg_type`, `frame.seq_num`,
    `frame.param`, `frame.payload`, `frame.payload_len`, `frame.hdr`,
    `frame.raw`) and 2-tuple unpacking (`hdr, payload = frame`).
    """

    __slots__ = ("hdr", "payload", "raw")

    def __init__(
        self,
        hdr: FrameHeader,
        payload: bytes = b"",
        raw: bytes | None = None,
    ) -> None:
        self.hdr = hdr
        self.payload = bytes(payload)
        if raw is None:
            raw = (
                HEADER_STRUCT.pack(
                    hdr.magic,
                    hdr.version,
                    hdr.msg_type,
                    hdr.seq_num,
                    hdr.payload_len,
                    hdr.param,
                )
                + self.payload
            )
        self.raw = bytes(raw)

    @property
    def magic(self) -> int:
        return self.hdr.magic

    @property
    def version(self) -> int:
        return self.hdr.version

    @property
    def msg_type(self) -> int:
        return self.hdr.msg_type

    @property
    def seq_num(self) -> int:
        return self.hdr.seq_num

    @property
    def payload_len(self) -> int:
        return self.hdr.payload_len

    @property
    def param(self) -> int:
        return self.hdr.param

    def __iter__(self):
        yield self.hdr
        yield self.payload

    def __len__(self) -> int:
        return 2

    def __getitem__(self, index: int):
        if index == 0:
            return self.hdr
        if index == 1:
            return self.payload
        raise IndexError("Frame tuple unpacking only supports (hdr, payload)")

    def __bytes__(self) -> bytes:
        return self.raw

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Frame):
            return self.hdr == other.hdr and self.payload == other.payload
        if isinstance(other, (bytes, bytearray, memoryview)):
            return self.raw == bytes(other)
        return False

    def __repr__(self) -> str:
        return (
            f"Frame(msg_type=0x{self.msg_type:02X} ({proto_msg_name(self.msg_type)}), "
            f"seq_num={self.seq_num}, param={self.param}, "
            f"payload_len={self.payload_len})"
        )


# ============================================================================
# 6. PROTOCOL EXCEPTIONS
# ============================================================================


class ProtocolError(ValueError):
    """Base exception for PodFlow framing and validation errors."""

    def __init__(
        self,
        message: str,
        *,
        err_code: int | None = None,
        recv_rc: int = RECV_ERR_IO,
        ws_close_code: int = 4901,
        hdr: FrameHeader | None = None,
    ) -> None:
        super().__init__(message)
        self.err_code = err_code
        self.recv_rc = recv_rc
        self.ws_close_code = ws_close_code
        self.hdr = hdr


class BadMagicError(ProtocolError):
    """Raised when header magic != 0x5753 (ERR_BAD_MAGIC = 100)."""

    def __init__(self, magic: int, hdr: FrameHeader | None = None) -> None:
        super().__init__(
            f"Bad magic: expected 0x{PROTO_MAGIC:04X}, got 0x{magic:04X}",
            err_code=ERR_BAD_MAGIC,
            recv_rc=RECV_ERR_BAD_MAGIC,
            ws_close_code=4100,
            hdr=hdr,
        )


class BadVersionError(ProtocolError):
    """Raised when header version != 1 (ERR_BAD_VERSION = 101)."""

    def __init__(self, version: int, hdr: FrameHeader | None = None) -> None:
        super().__init__(
            f"Bad version: expected {PROTO_VERSION}, got {version}",
            err_code=ERR_BAD_VERSION,
            recv_rc=RECV_ERR_BAD_VERSION,
            ws_close_code=4101,
            hdr=hdr,
        )


class PayloadTooLargeError(ProtocolError):
    """Raised when payload_len > MAX_PAYLOAD_SIZE or buf_cap (ERR_PAYLOAD_TOO_LARGE = 103)."""

    def __init__(
        self,
        payload_len: int,
        limit: int = MAX_PAYLOAD_SIZE,
        hdr: FrameHeader | None = None,
    ) -> None:
        super().__init__(
            f"Payload too large: {payload_len} > {limit}",
            err_code=ERR_PAYLOAD_TOO_LARGE,
            recv_rc=RECV_ERR_PAYLOAD_LEN,
            ws_close_code=4103,
            hdr=hdr,
        )


class TruncatedFrameError(ProtocolError, EOFError):
    """Raised when EOF occurs mid-header or mid-payload (RECV_ERR_IO = -1)."""

    def __init__(self, message: str, hdr: FrameHeader | None = None) -> None:
        super().__init__(
            message,
            err_code=None,
            recv_rc=RECV_ERR_IO,
            ws_close_code=4901,
            hdr=hdr,
        )


# ============================================================================
# 7. PUBLIC API FUNCTIONS
# ============================================================================

PayloadType = Union[bytes, bytearray, memoryview, str, None]


def _coerce_payload(payload: PayloadType) -> bytes:
    if payload is None:
        return b""
    if isinstance(payload, str):
        return payload.encode("utf-8")
    if isinstance(payload, bytes):
        return payload
    if isinstance(payload, (bytearray, memoryview)):
        return bytes(payload)
    raise TypeError(f"Unsupported payload type: {type(payload).__name__}")


def pack_header(
    msg_type: int,
    seq_num: int = 0,
    payload_len: int = 0,
    param: int = 0,
    *,
    magic: int = PROTO_MAGIC,
    version: int = PROTO_VERSION,
) -> bytes:
    """Serialize a 16-byte big-endian PodFlow frame header."""
    if not (0 <= magic <= 0xFFFF):
        raise ValueError(f"magic out of u16 range: {magic}")
    if not (0 <= version <= 0xFF):
        raise ValueError(f"version out of u8 range: {version}")
    if not (0 <= msg_type <= 0xFF):
        raise ValueError(f"msg_type out of u8 range: {msg_type}")
    if not (0 <= seq_num <= 0xFFFFFFFF):
        raise ValueError(f"seq_num out of u32 range: {seq_num}")
    if not (0 <= payload_len <= MAX_PAYLOAD_SIZE):
        raise PayloadTooLargeError(payload_len, MAX_PAYLOAD_SIZE)
    if not (0 <= param <= 0xFFFFFFFF):
        raise ValueError(f"param out of u32 range: {param}")

    return HEADER_STRUCT.pack(magic, version, msg_type, seq_num, payload_len, param)


def pack_frame(
    msg_type: int,
    seq_num: int = 0,
    param: int = 0,
    payload: PayloadType = b"",
) -> bytes:
    """Serialize a complete PodFlow frame (16-byte header + payload) into bytes."""
    raw_payload = _coerce_payload(payload)
    payload_len = len(raw_payload)
    if payload_len > MAX_PAYLOAD_SIZE:
        raise PayloadTooLargeError(payload_len, MAX_PAYLOAD_SIZE)

    raw_hdr = pack_header(msg_type, seq_num, payload_len, param)
    return raw_hdr + raw_payload


def validate_header(hdr: FrameHeader, *, buf_cap: int = MAX_PAYLOAD_SIZE) -> None:
    """Validate protocol invariants on an unpacked 16-byte header."""
    if hdr.magic != PROTO_MAGIC:
        raise BadMagicError(hdr.magic, hdr=hdr)
    if hdr.version != PROTO_VERSION:
        raise BadVersionError(hdr.version, hdr=hdr)
    limit = min(MAX_PAYLOAD_SIZE, buf_cap)
    if hdr.payload_len > limit:
        raise PayloadTooLargeError(hdr.payload_len, limit, hdr=hdr)


def unpack_header(
    data: Union[bytes, bytearray, memoryview],
    *,
    validate: bool = True,
    buf_cap: int = MAX_PAYLOAD_SIZE,
) -> FrameHeader:
    """Unpack and optionally validate a 16-byte big-endian frame header."""
    raw = bytes(data)
    if len(raw) < FRAME_HEADER_SIZE:
        raise TruncatedFrameError(
            f"Header too short: expected {FRAME_HEADER_SIZE} bytes, got {len(raw)}"
        )
    magic, version, msg_type, seq_num, payload_len, param = HEADER_STRUCT.unpack(
        raw[:FRAME_HEADER_SIZE]
    )
    hdr = FrameHeader(
        magic=magic,
        version=version,
        msg_type=msg_type,
        seq_num=seq_num,
        payload_len=payload_len,
        param=param,
    )
    if validate:
        validate_header(hdr, buf_cap=buf_cap)
    return hdr


def unpack_frame(
    data: Union[bytes, bytearray, memoryview],
    *,
    validate: bool = True,
    buf_cap: int = MAX_PAYLOAD_SIZE,
) -> Frame:
    """Unpack a complete binary frame buffer (header + exact payload)."""
    raw = bytes(data)
    hdr = unpack_header(raw, validate=validate, buf_cap=buf_cap)
    expected_total = FRAME_HEADER_SIZE + hdr.payload_len
    if len(raw) < expected_total:
        raise TruncatedFrameError(
            f"Truncated payload: expected {hdr.payload_len} bytes, "
            f"got {len(raw) - FRAME_HEADER_SIZE}",
            hdr=hdr,
        )
    payload = raw[FRAME_HEADER_SIZE:expected_total]
    return Frame(hdr=hdr, payload=payload, raw=raw[:expected_total])


async def recv_frame(
    reader: asyncio.StreamReader,
    *,
    buf_cap: int = MAX_PAYLOAD_SIZE,
) -> Frame | None:
    """Asynchronously read and validate a single PodFlow frame from a StreamReader.

    Mirrors C `recv_frame()` semantics:
      - Returns `None` on clean EOF before any header byte arrives (`RECV_EOF = 0`).
      - Raises `TruncatedFrameError` on mid-header or mid-payload EOF (`RECV_ERR_IO = -1`).
      - Raises `BadMagicError` if `magic != 0x5753` (`RECV_ERR_BAD_MAGIC = -2`).
      - Raises `BadVersionError` if `version != 1` (`RECV_ERR_BAD_VERSION = -3`).
      - Raises `PayloadTooLargeError` if `payload_len > MAX_PAYLOAD_SIZE` or
        `payload_len > buf_cap` (`RECV_ERR_PAYLOAD_LEN = -4`) WITHOUT draining
        the untrusted payload bytes from `reader`.
    """
    try:
        raw_hdr = await reader.readexactly(FRAME_HEADER_SIZE)
    except asyncio.IncompleteReadError as exc:
        if len(exc.partial) == 0:
            return None
        raise TruncatedFrameError(
            f"Truncated frame header: expected {FRAME_HEADER_SIZE} bytes, "
            f"got {len(exc.partial)}"
        ) from exc

    hdr = unpack_header(raw_hdr, validate=True, buf_cap=buf_cap)

    if hdr.payload_len > 0:
        try:
            payload = await reader.readexactly(hdr.payload_len)
        except asyncio.IncompleteReadError as exc:
            raise TruncatedFrameError(
                f"Truncated frame payload: expected {hdr.payload_len} bytes, "
                f"got {len(exc.partial)}",
                hdr=hdr,
            ) from exc
    else:
        payload = b""

    return Frame(hdr=hdr, payload=payload, raw=raw_hdr + payload)


def proto_msg_name(msg_type: int) -> str:
    """Return human-readable opcode name (mirroring C proto_msg_name)."""
    names = {
        MSG_HELLO: "HELLO",
        MSG_WELCOME: "WELCOME",
        MSG_LIST: "LIST",
        MSG_PLAY: "PLAY",
        MSG_PAUSE: "PAUSE",
        MSG_SEEK: "SEEK",
        MSG_STOP: "STOP",
        MSG_DATA: "DATA",
        MSG_END: "END",
        MSG_ACK: "ACK",
        MSG_ECHO: "ECHO",
        MSG_ERROR: "ERROR",
    }
    return names.get(msg_type, "UNKNOWN_MSG")


def proto_err_name(code: int) -> str:
    """Return human-readable error code name (mirroring C proto_err_name)."""
    names = {
        ERR_NONE: "NONE",
        ERR_BAD_MAGIC: "BAD_MAGIC",
        ERR_BAD_VERSION: "BAD_VERSION",
        ERR_UNKNOWN_CMD: "UNKNOWN_CMD",
        ERR_PAYLOAD_TOO_LARGE: "PAYLOAD_TOO_LARGE",
        ERR_FILE_NOT_FOUND: "FILE_NOT_FOUND",
        ERR_INVALID_WAV: "INVALID_WAV",
        ERR_SEEK_OUT_OF_RANGE: "SEEK_OUT_OF_RANGE",
        ERR_INVALID_STATE: "INVALID_STATE",
        ERR_INTERNAL_SERVER: "INTERNAL_SERVER",
        ERR_SERVER_BUSY: "SERVER_BUSY",
    }
    return names.get(code, "UNKNOWN_ERR")
