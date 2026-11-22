// Cermin include/protocol.h
export const PROTO_MAGIC = 0x5753;
export const PROTO_VERSION = 1;
export const FRAME_HEADER_SIZE = 16;
export const PCM_CHUNK_SIZE = 8820;
export const MSG = {
  HELLO: 0x01, WELCOME: 0x02, LIST: 0x03, PLAY: 0x04, PAUSE: 0x05,
  SEEK: 0x06, STOP: 0x07, DATA: 0x08, END: 0x09, ACK: 0x0A, ECHO: 0x0B,
  ERROR: 0xFF,
};
