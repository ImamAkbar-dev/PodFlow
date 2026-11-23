/**
 * PodFlow Binary Framing Protocol (v1.1) - Browser Implementation
 * 16-Byte Big-Endian Header:
 *   [0..1]   uint16 magic       = 0x5753 ("WS")
 *   [2]      uint8  version     = 1
 *   [3]      uint8  msg_type
 *   [4..7]   uint32 seq_num
 *   [8..11]  uint32 payload_len
 *   [12..15] uint32 param
 */

export const PROTOCOL_MAGIC = 0x5753;
export const PROTOCOL_VERSION = 1;
export const HEADER_SIZE = 16;
export const MAX_PAYLOAD_LEN = 65536;
export const CHUNK_BYTES = 8820;
export const CHUNKS_PER_SEC = 20;

export const MSG_TYPE = Object.freeze({
    HELLO:   0x01,
    WELCOME: 0x02,
    LIST:    0x03,
    PLAY:    0x04,
    PAUSE:   0x05,
    SEEK:    0x06,
    STOP:    0x07,
    DATA:    0x08,
    END:     0x09,
    ACK:     0x0A,
    ECHO:    0x0B,
    ERROR:   0xFF
});

export const ERR_CODE = Object.freeze({
    BAD_MAGIC:         100,
    BAD_VERSION:       101,
    UNKNOWN_TYPE:      102,
    PAYLOAD_TOO_LARGE: 103,
    FILE_NOT_FOUND:    200,
    INVALID_FILENAME:  201,
    SEEK_OUT_OF_RANGE: 202,
    INVALID_STATE:     300,
    SERVER_BUSY:       503
});

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8");

/**
 * Pack a PodFlow frame into an ArrayBuffer (16-byte Big-Endian header + payload).
 * @param {number} msgType
 * @param {number} seqNum
 * @param {string|Uint8Array|ArrayBuffer|null} payload
 * @param {number} param
 * @returns {ArrayBuffer}
 */
export function packFrame(msgType, seqNum = 0, payload = null, param = 0) {
    let payloadBytes;
    if (payload === null || payload === undefined) {
        payloadBytes = new Uint8Array(0);
    } else if (typeof payload === "string") {
        payloadBytes = textEncoder.encode(payload);
    } else if (payload instanceof Uint8Array) {
        payloadBytes = payload;
    } else if (payload instanceof ArrayBuffer) {
        payloadBytes = new Uint8Array(payload);
    } else {
        throw new TypeError("Unsupported payload type");
    }

    if (payloadBytes.byteLength > MAX_PAYLOAD_LEN) {
        throw new RangeError(`Payload exceeds ${MAX_PAYLOAD_LEN} bytes`);
    }

    const buffer = new ArrayBuffer(HEADER_SIZE + payloadBytes.byteLength);
    const view = new DataView(buffer);

    view.setUint16(0, PROTOCOL_MAGIC, false);          // Big-Endian
    view.setUint8(2, PROTOCOL_VERSION);
    view.setUint8(3, msgType & 0xFF);
    view.setUint32(4, seqNum >>> 0, false);            // Big-Endian
    view.setUint32(8, payloadBytes.byteLength, false); // Big-Endian
    view.setUint32(12, param >>> 0, false);            // Big-Endian

    if (payloadBytes.byteLength > 0) {
        new Uint8Array(buffer, HEADER_SIZE).set(payloadBytes);
    }

    return buffer;
}

/**
 * Unpack a binary ArrayBuffer received from WebSocket into a PodFlow frame object.
 * @param {ArrayBuffer} buffer
 */
export function unpackFrame(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < HEADER_SIZE) {
        throw new Error(`Frame too short: ${buffer ? buffer.byteLength : 0} bytes`);
    }

    const view = new DataView(buffer);
    const magic = view.getUint16(0, false);
    const version = view.getUint8(2);
    const msgType = view.getUint8(3);
    const seqNum = view.getUint32(4, false);
    const payloadLen = view.getUint32(8, false);
    const param = view.getUint32(12, false);

    if (magic !== PROTOCOL_MAGIC) {
        throw new Error(`Invalid magic: 0x${magic.toString(16)}`);
    }
    if (version !== PROTOCOL_VERSION) {
        throw new Error(`Unsupported version: ${version}`);
    }
    if (buffer.byteLength !== HEADER_SIZE + payloadLen) {
        throw new Error(`Length mismatch: expected ${HEADER_SIZE + payloadLen}, got ${buffer.byteLength}`);
    }

    const payload = new Uint8Array(buffer, HEADER_SIZE, payloadLen);
    return {
        magic,
        version,
        msgType,
        seqNum,
        payloadLen,
        param,
        payload,
        textPayload() {
            return textDecoder.decode(payload);
        }
    };
}