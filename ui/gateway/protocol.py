"""Pack/unpack frame PodFlow v1.1 (cermin dari include/protocol.h).

Header 16 byte, big-endian. Logika pack/unpack belum ditulis; cocokkan dulu
dengan docs/protocol_spec.md bagian 4.
"""
PROTO_MAGIC = 0x5753
PROTO_VERSION = 1
FRAME_HEADER_SIZE = 16
PCM_CHUNK_SIZE = 8820
WAV_HEADER_SIZE = 44
MAX_PAYLOAD_SIZE = 65536
DEFAULT_PORT = 8888

MSG_HELLO, MSG_WELCOME, MSG_LIST, MSG_PLAY = 0x01, 0x02, 0x03, 0x04
MSG_PAUSE, MSG_SEEK, MSG_STOP, MSG_DATA = 0x05, 0x06, 0x07, 0x08
MSG_END, MSG_ACK, MSG_ECHO, MSG_ERROR = 0x09, 0x0A, 0x0B, 0xFF

# TODO: pack_frame(), recv_frame()
