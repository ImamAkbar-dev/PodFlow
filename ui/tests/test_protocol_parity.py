"""Unit tests untuk paritas konstanta protocol.h vs protocol.py vs protocol.js
serta pengujian pack_frame() dan recv_frame() (frame valid & rusak).

Owner: Imam (Anggota 2 - Inti Gateway: Protokol & Sesi)
"""

from __future__ import annotations

import asyncio
import re
import sys
import unittest
from pathlib import Path

UI_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = UI_DIR.parent
if str(UI_DIR) not in sys.path:
    sys.path.insert(0, str(UI_DIR))

from gateway import protocol  # noqa: E402
from gateway.protocol import (  # noqa: E402
    BadMagicError,
    BadVersionError,
    FRAME_HEADER_SIZE,
    MAX_PAYLOAD_SIZE,
    MSG_ACK,
    MSG_DATA,
    MSG_ECHO,
    MSG_HELLO,
    MSG_PLAY,
    MSG_SEEK,
    PCM_CHUNK_SIZE,
    PROTO_MAGIC,
    PROTO_VERSION,
    PayloadTooLargeError,
    TruncatedFrameError,
    WAV_HEADER_SIZE,
    pack_frame,
    pack_header,
    proto_err_name,
    proto_msg_name,
    recv_frame,
    unpack_frame,
    unpack_header,
)


def parse_c_protocol_header(header_path: Path) -> dict[str, int | str]:
    """Extract #define constants and enum values from include/protocol.h."""
    text = header_path.read_text(encoding="utf-8")
    # Strip C block comments and line comments
    text_no_comments = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text_no_comments = re.sub(r"//.*?$", "", text_no_comments, flags=re.MULTILINE)

    constants: dict[str, int | str] = {}

    # 1. #define NAME VALUE
    define_re = re.compile(
        r"^\s*#define\s+([A-Z0-9_]+)\s+(.+?)\s*$", re.MULTILINE
    )
    for match in define_re.finditer(text_no_comments):
        name, raw_val = match.group(1), match.group(2).strip()
        if name == "PODFLOW_PROTOCOL_H":
            continue
        # Strip outer parentheses like (-1)
        if raw_val.startswith("(") and raw_val.endswith(")"):
            raw_val = raw_val[1:-1].strip()
        # String literal
        if raw_val.startswith('"') and raw_val.endswith('"'):
            constants[name] = raw_val[1:-1]
            continue
        # Integer literal (hex or decimal with optional U/L suffix)
        num_match = re.match(r"^(-?(?:0x[0-9a-fA-F]+|\d+))[uUlL]*$", raw_val)
        if num_match:
            constants[name] = int(num_match.group(1), 0)

    # 2. typedef enum { ... } proto_msg_t / proto_err_t;
    enum_re = re.compile(r"typedef\s+enum\s*\{(.*?)\}\s*\w+\s*;", re.DOTALL)
    entry_re = re.compile(r"([A-Z0-9_]+)\s*=\s*(-?(?:0x[0-9a-fA-F]+|\d+))")
    for enum_match in enum_re.finditer(text_no_comments):
        body = enum_match.group(1)
        for entry in entry_re.finditer(body):
            constants[entry.group(1)] = int(entry.group(2), 0)

    return constants


def parse_js_protocol(js_path: Path) -> tuple[dict[str, int | str], dict[str, dict[str, int]]]:
    """Extract top-level `export const` values and object maps from web/js/protocol.js."""
    text = js_path.read_text(encoding="utf-8")
    text_no_comments = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text_no_comments = re.sub(r"//.*?$", "", text_no_comments, flags=re.MULTILINE)

    scalars: dict[str, int | str] = {}
    objects: dict[str, dict[str, int]] = {}

    scalar_re = re.compile(
        r"export\s+const\s+([A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|-?\d+|\"[^\"]*\"|'[^']*')\s*;"
    )
    for m in scalar_re.finditer(text_no_comments):
        name, val = m.group(1), m.group(2)
        if (val.startswith('"') and val.endswith('"')) or (
            val.startswith("'") and val.endswith("'")
        ):
            scalars[name] = val[1:-1]
        else:
            scalars[name] = int(val, 0)

    obj_re = re.compile(
        r"export\s+const\s+([A-Z0-9_]+)\s*=\s*\{(.*?)\}\s*;", re.DOTALL
    )
    pair_re = re.compile(r"([A-Z0-9_]+)\s*:\s*(0x[0-9a-fA-F]+|-?\d+)")
    for m in obj_re.finditer(text_no_comments):
        obj_name, body = m.group(1), m.group(2)
        objects[obj_name] = {
            pm.group(1): int(pm.group(2), 0) for pm in pair_re.finditer(body)
        }

    return scalars, objects


class ProtocolParityTest(unittest.TestCase):
    """Verify protocol constants parity across C, Python gateway, and browser JS."""

    def setUp(self) -> None:
        self.c_header_path = REPO_ROOT / "include" / "protocol.h"
        self.js_proto_path = UI_DIR / "web" / "js" / "protocol.js"
        self.assertTrue(
            self.c_header_path.is_file(),
            f"Missing C header: {self.c_header_path}",
        )
        self.assertTrue(
            self.js_proto_path.is_file(),
            f"Missing JS file: {self.js_proto_path}",
        )
        self.c_consts = parse_c_protocol_header(self.c_header_path)

    def test_constants_match_c_header_and_python(self) -> None:
        """Every constant and enum value in include/protocol.h must match gateway/protocol.py."""
        self.assertGreaterEqual(len(self.c_consts), 45)
        for name, c_val in self.c_consts.items():
            self.assertTrue(
                hasattr(protocol, name),
                f"gateway/protocol.py is missing constant '{name}' from include/protocol.h",
            )
            py_val = getattr(protocol, name)
            self.assertEqual(
                py_val,
                c_val,
                f"Mismatch for {name}: protocol.py={py_val!r} vs protocol.h={c_val!r}",
            )

    def test_constants_match_js_protocol(self) -> None:
        """Every constant exported in web/js/protocol.js must match include/protocol.h."""
        scalars, objects = parse_js_protocol(self.js_proto_path)
        self.assertIn("PROTO_MAGIC", scalars)
        self.assertIn("PROTO_VERSION", scalars)
        self.assertIn("FRAME_HEADER_SIZE", scalars)
        self.assertIn("PCM_CHUNK_SIZE", scalars)

        for name, js_val in scalars.items():
            self.assertIn(name, self.c_consts, f"Unknown scalar {name} in protocol.js")
            self.assertEqual(js_val, self.c_consts[name], f"JS mismatch for {name}")
            self.assertEqual(js_val, getattr(protocol, name))

        self.assertIn("MSG", objects)
        expected_msgs = [
            "HELLO",
            "WELCOME",
            "LIST",
            "PLAY",
            "PAUSE",
            "SEEK",
            "STOP",
            "DATA",
            "END",
            "ACK",
            "ECHO",
            "ERROR",
        ]
        for msg_name in expected_msgs:
            self.assertIn(msg_name, objects["MSG"])
            self.assertEqual(
                objects["MSG"][msg_name],
                self.c_consts[f"MSG_{msg_name}"],
                f"JS MSG.{msg_name} mismatch",
            )

        if "ERR" in objects:
            for err_name, err_val in objects["ERR"].items():
                self.assertEqual(err_val, self.c_consts[f"ERR_{err_name}"])

    def test_worked_hex_dumps_from_spec(self) -> None:
        """Verify byte-for-byte big-endian layout against Section 11 Worked Hex Dumps."""
        # 1) HELLO frame: "cli-1" (5 B), seq=0, param=0
        hello = pack_frame(MSG_HELLO, seq_num=0, param=0, payload="cli-1")
        self.assertEqual(
            hello,
            bytes.fromhex("57 53 01 01 00 00 00 00 00 00 00 05 00 00 00 00 63 6C 69 2D 31"),
        )

        # 2) PLAY frame: filename="sample_30s.wav" (14 B), start_chunk=1, seq=1
        play = pack_frame(MSG_PLAY, seq_num=1, param=1, payload="sample_30s.wav")
        self.assertEqual(
            play,
            bytes.fromhex(
                "57 53 01 04 00 00 00 01 00 00 00 0E 00 00 00 01 "
                "73 61 6D 70 6C 65 5F 33 30 73 2E 77 61 76"
            ),
        )

        # 3) ACK frame: seq=1, param=1, payload_len=0
        ack = pack_frame(MSG_ACK, seq_num=1, param=1, payload=b"")
        self.assertEqual(
            ack,
            bytes.fromhex("57 53 01 0A 00 00 00 01 00 00 00 00 00 00 00 01"),
        )

        # 4) SEEK frame: seq=4, target=100 (0x64), payload_len=0
        seek = pack_frame(MSG_SEEK, seq_num=4, param=100, payload=b"")
        self.assertEqual(
            seek,
            bytes.fromhex("57 53 01 06 00 00 00 04 00 00 00 00 00 00 00 64"),
        )

        # 5) DATA frame header: seq=100 (0x64), payload_len=8820 (0x2274), param=0
        data_hdr = pack_header(MSG_DATA, seq_num=100, payload_len=8820, param=0)
        self.assertEqual(
            data_hdr,
            bytes.fromhex("57 53 01 08 00 00 00 64 00 00 22 74 00 00 00 00"),
        )

    def test_pack_and_unpack_boundaries_and_rejections(self) -> None:
        """Test boundary payload sizes (0..65536) and invalid input rejections."""
        for size in (0, 1, 15, 16, WAV_HEADER_SIZE, PCM_CHUNK_SIZE, MAX_PAYLOAD_SIZE):
            payload = bytes((i & 0xFF) for i in range(size))
            raw = pack_frame(MSG_ECHO, seq_num=size, param=size * 3, payload=payload)
            self.assertEqual(len(raw), FRAME_HEADER_SIZE + size)

            frame = unpack_frame(raw)
            hdr, unpacked_payload = frame  # verify 2-tuple unpacking
            self.assertEqual(hdr.magic, PROTO_MAGIC)
            self.assertEqual(hdr.version, PROTO_VERSION)
            self.assertEqual(hdr.msg_type, MSG_ECHO)
            self.assertEqual(hdr.seq_num, size)
            self.assertEqual(hdr.payload_len, size)
            self.assertEqual(hdr.param, size * 3)
            self.assertEqual(unpacked_payload, payload)
            self.assertEqual(bytes(frame), raw)

        # Reject payload > 65536
        with self.assertRaises(PayloadTooLargeError):
            pack_frame(MSG_ECHO, seq_num=1, param=0, payload=b"x" * (MAX_PAYLOAD_SIZE + 1))

        # Reject out-of-range fields
        with self.assertRaises(ValueError):
            pack_frame(0x100, seq_num=0, param=0)
        with self.assertRaises(ValueError):
            pack_frame(MSG_ECHO, seq_num=-1, param=0)
        with self.assertRaises(ValueError):
            pack_frame(MSG_ECHO, seq_num=0, param=0x1_0000_0000)

        # Helper name lookups
        self.assertEqual(proto_msg_name(MSG_PLAY), "PLAY")
        self.assertEqual(proto_msg_name(0x99), "UNKNOWN_MSG")
        self.assertEqual(proto_err_name(503), "SERVER_BUSY")
        self.assertEqual(proto_err_name(999), "UNKNOWN_ERR")

    def test_async_recv_frame_valid_and_corrupted(self) -> None:
        """Test async recv_frame() on valid frames, clean EOF, truncation, and framing errors."""

        async def run_scenarios() -> None:
            # 1. Clean EOF -> returns None
            r_eof = asyncio.StreamReader()
            r_eof.feed_eof()
            self.assertIsNone(await recv_frame(r_eof))

            # 2. Truncated header (7 bytes before EOF) -> TruncatedFrameError
            r_trunc_hdr = asyncio.StreamReader()
            r_trunc_hdr.feed_data(b"WS\x01\x01\x00\x00\x00")
            r_trunc_hdr.feed_eof()
            with self.assertRaises(TruncatedFrameError) as ctx_th:
                await recv_frame(r_trunc_hdr)
            self.assertEqual(ctx_th.exception.recv_rc, protocol.RECV_ERR_IO)

            # 3. Truncated payload (declared 10 bytes, only 4 bytes sent) -> TruncatedFrameError
            r_trunc_pay = asyncio.StreamReader()
            r_trunc_pay.feed_data(pack_header(MSG_ECHO, seq_num=1, payload_len=10, param=0) + b"abcd")
            r_trunc_pay.feed_eof()
            with self.assertRaises(TruncatedFrameError):
                await recv_frame(r_trunc_pay)

            # 4. Bad magic (0xDEAD) -> BadMagicError (code 100, ws 4100)
            r_magic = asyncio.StreamReader()
            bad_magic_hdr = protocol.HEADER_STRUCT.pack(0xDEAD, PROTO_VERSION, MSG_HELLO, 0, 0, 0)
            r_magic.feed_data(bad_magic_hdr)
            with self.assertRaises(BadMagicError) as ctx_bm:
                await recv_frame(r_magic)
            self.assertEqual(ctx_bm.exception.err_code, protocol.ERR_BAD_MAGIC)
            self.assertEqual(ctx_bm.exception.recv_rc, protocol.RECV_ERR_BAD_MAGIC)
            self.assertEqual(ctx_bm.exception.ws_close_code, 4100)

            # 5. Bad version (2) -> BadVersionError (code 101, ws 4101)
            r_ver = asyncio.StreamReader()
            bad_ver_hdr = protocol.HEADER_STRUCT.pack(PROTO_MAGIC, 2, MSG_HELLO, 0, 0, 0)
            r_ver.feed_data(bad_ver_hdr)
            with self.assertRaises(BadVersionError) as ctx_bv:
                await recv_frame(r_ver)
            self.assertEqual(ctx_bv.exception.err_code, protocol.ERR_BAD_VERSION)
            self.assertEqual(ctx_bv.exception.recv_rc, protocol.RECV_ERR_BAD_VERSION)
            self.assertEqual(ctx_bv.exception.ws_close_code, 4101)

            # 6. Oversize payload_len (70000 > 65536) -> PayloadTooLargeError WITHOUT draining!
            r_big = asyncio.StreamReader()
            big_hdr = protocol.HEADER_STRUCT.pack(PROTO_MAGIC, PROTO_VERSION, MSG_ECHO, 9, 70000, 0)
            sentinel = b"UNTOUCHED_BYTES"
            r_big.feed_data(big_hdr + sentinel)
            with self.assertRaises(PayloadTooLargeError) as ctx_bp:
                await recv_frame(r_big)
            self.assertEqual(ctx_bp.exception.err_code, protocol.ERR_PAYLOAD_TOO_LARGE)
            self.assertEqual(ctx_bp.exception.recv_rc, protocol.RECV_ERR_PAYLOAD_LEN)
            self.assertEqual(ctx_bp.exception.ws_close_code, 4103)
            # Prove recv_frame did NOT drain the payload bytes after the 16-byte header
            remaining = await r_big.readexactly(len(sentinel))
            self.assertEqual(remaining, sentinel)

            # 7. Fragmented single-byte stream of multiple valid frames
            r_frag = asyncio.StreamReader()
            f1_raw = pack_frame(MSG_HELLO, seq_num=1, param=0, payload="m2-test")
            f2_raw = pack_frame(MSG_ECHO, seq_num=2, param=77, payload=b"\x00\xff" * 64)

            async def drip_bytes() -> None:
                for b in f1_raw + f2_raw:
                    r_frag.feed_data(bytes([b]))
                    await asyncio.sleep(0)
                r_frag.feed_eof()

            drip_task = asyncio.create_task(drip_bytes())
            f1 = await recv_frame(r_frag)
            f2 = await recv_frame(r_frag)
            f3 = await recv_frame(r_frag)
            await drip_task

            self.assertIsNotNone(f1)
            self.assertIsNotNone(f2)
            self.assertIsNone(f3)
            assert f1 is not None and f2 is not None
            self.assertEqual(f1.raw, f1_raw)
            self.assertEqual(f2.raw, f2_raw)

        asyncio.run(run_scenarios())


if __name__ == "__main__":
    unittest.main()
