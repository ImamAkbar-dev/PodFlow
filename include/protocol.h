/**
 * @file protocol.h
 * @brief Frozen Wire Protocol Contract for PodFlow (Concurrent Audio Streamer).
 *
 * Owner: Member 2 (Protocol & Framing)
 * Standard: C11, POSIX Sockets, Big-Endian (Network Byte Order) Wire Format.
 */

#ifndef PODFLOW_PROTOCOL_H
#define PODFLOW_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== *
 * 1. PROTOCOL CONSTANTS                                                      *
 * ========================================================================== */

#define PROTO_MAGIC       0x5753U   /* "WS" (0x57='W', 0x53='S') in big-endian */
#define PROTO_VERSION     1U        /* Protocol version 1                      */
#define DEFAULT_PORT      8888U     /* Default TCP listening port              */

#define WAV_HEADER_SIZE   44U       /* Canonical RIFF/WAV PCM header size (B)  */
#define PCM_CHUNK_SIZE    8820U     /* 44100 Hz * 2 ch * 2 B * 0.05 s (50 ms)  */
#define MAX_PAYLOAD_SIZE  65536U    /* Hard upper bound on frame payload (64K) */
#define FRAME_HEADER_SIZE 16U       /* Wire header size in bytes               */

#define WELCOME_BANNER    "wav-streamer/1"

/* ========================================================================== *
 * 2. MESSAGE TYPES (u8 on wire)                                              *
 *    Both unprefixed (contract) and MSG_-prefixed aliases are provided so    *
 *    all team members compile cleanly regardless of prefix convention.       *
 * ========================================================================== */

typedef enum {
    /* Contract names */
    HELLO   = 0x01,
    WELCOME = 0x02,
    LIST    = 0x03,
    PLAY    = 0x04,
    PAUSE   = 0x05, /* Reserved/Planned for Session 2 (returns 300 in S1) */
    SEEK    = 0x06, /* Reserved/Planned for Session 2 (returns 300 in S1) */
    STOP    = 0x07, /* Reserved/Planned for Session 2 (returns 300 in S1) */
    DATA    = 0x08,
    END     = 0x09,
    ACK     = 0x0A,
    ECHO    = 0x0B,
    ERROR   = 0xFF,

    /* Prefixed aliases for defensive namespace safety */
    MSG_HELLO   = 0x01,
    MSG_WELCOME = 0x02,
    MSG_LIST    = 0x03,
    MSG_PLAY    = 0x04,
    MSG_PAUSE   = 0x05,
    MSG_SEEK    = 0x06,
    MSG_STOP    = 0x07,
    MSG_DATA    = 0x08,
    MSG_END     = 0x09,
    MSG_ACK     = 0x0A,
    MSG_ECHO    = 0x0B,
    MSG_ERROR   = 0xFF
} proto_msg_t;

/* ========================================================================== *
 * 3. ERROR CODES (carried in frame_hdr_t.param of ERROR frames, u32)         *
 * ========================================================================== */

typedef enum {
    /* Contract names */
    NONE              = 0,
    BAD_MAGIC         = 100,
    BAD_VERSION       = 101,
    UNKNOWN_CMD       = 102,
    PAYLOAD_TOO_LARGE = 103,
    FILE_NOT_FOUND    = 200,
    INVALID_WAV       = 201,
    SEEK_OUT_OF_RANGE = 202,
    INVALID_STATE     = 300,
    INTERNAL_SERVER   = 500,

    /* Prefixed aliases */
    ERR_NONE              = 0,
    ERR_BAD_MAGIC         = 100,
    ERR_BAD_VERSION       = 101,
    ERR_UNKNOWN_CMD       = 102,
    ERR_PAYLOAD_TOO_LARGE = 103,
    ERR_FILE_NOT_FOUND    = 200,
    ERR_INVALID_WAV       = 201,
    ERR_SEEK_OUT_OF_RANGE = 202,
    ERR_INVALID_STATE     = 300,
    ERR_INTERNAL_SERVER   = 500
} proto_err_t;

/* ========================================================================== *
 * 4. WIRE HEADER STRUCTURE (16 bytes, packed, big-endian on wire)            *
 *                                                                            *
 *   Offset  Size  Type   Field        Description                            *
 *   ------  ----  -----  -----------  -------------------------------------  *
 *   0       2     u16    magic        0x5753 ("WS")                          *
 *   2       1     u8     version      1                                      *
 *   3       1     u8     msg_type     proto_msg_t value                      *
 *   4       4     u32    seq_num      Sequence number                        *
 *   8       4     u32    payload_len  Payload byte length (0..65536)         *
 *   12      4     u32    param        Command/response parameter or err code *
 *                                                                            *
 * Session 2 Reserved Parameter Semantics (Documented Contract):              *
 *   - PLAY: param = start chunk index (0 = from beginning of file).          *
 *   - SEEK: param = target chunk number (>= 1);                              *
 *           byte offset in WAV file = 44 + (chunk - 1) * 8820.               *
 *   - PAUSE / STOP: param = 0, payload_len = 0.                              *
 * ========================================================================== */

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t seq_num;
    uint32_t payload_len;
    uint32_t param;
} frame_hdr_t;

_Static_assert(sizeof(frame_hdr_t) == 16, "frame_hdr_t must be exactly 16 bytes");

/* ========================================================================== *
 * 5. RETURN CODES FOR recv_frame()                                           *
 * ========================================================================== */

#define RECV_OK                 1   /* Frame header + payload read cleanly    */
#define RECV_EOF                0   /* Clean EOF before any header byte read  */
#define RECV_ERR_IO            (-1) /* I/O error or truncated header/payload  */
#define RECV_ERR_BAD_MAGIC     (-2) /* magic != PROTO_MAGIC (0x5753)          */
#define RECV_ERR_BAD_VERSION   (-3) /* version != PROTO_VERSION (1)           */
#define RECV_ERR_PAYLOAD_LEN   (-4) /* payload_len > MAX_PAYLOAD_SIZE/buf_cap */

/* ========================================================================== *
 * 6. PUBLIC API PROTOTYPES                                                   *
 * ========================================================================== */

/**
 * @brief Serialize and transmit a complete protocol frame in a single writen().
 *
 * Assembles the 16-byte big-endian header and up to MAX_PAYLOAD_SIZE bytes of
 * payload into a single thread-local stack buffer and invokes writen() once.
 *
 * @param fd          Connected socket file descriptor.
 * @param msg_type    Message type (e.g., HELLO, WELCOME, DATA, END, ERROR).
 * @param seq         32-bit sequence number (host byte order).
 * @param param       32-bit parameter field (host byte order).
 * @param payload     Pointer to payload bytes (may be NULL iff payload_len == 0).
 * @param payload_len Number of payload bytes to send (0 .. MAX_PAYLOAD_SIZE).
 * @return 0 on complete transmission; -1 on validation or I/O error (sets errno).
 */
int send_frame(int fd, uint8_t msg_type, uint32_t seq, uint32_t param,
               const void *payload, uint32_t payload_len);

/**
 * @brief Receive and validate a single protocol frame from a socket.
 *
 * Reads the 16-byte wire header using readn(), converts all multi-byte fields
 * from big-endian to host byte order into *hdr_out, validates magic, version,
 * and payload_len bounds, and reads payload_len bytes into payload_buf.
 *
 * IMPORTANT CONNECTION LIFECYCLE RULE FOR CALLERS:
 * - On return -2 (bad magic), -3 (bad version), or -4 (oversize payload_len):
 *   recv_frame() does NOT drain the payload from the socket. The caller MUST
 *   send the appropriate ERROR frame (if server) and CLOSE the socket fd
 *   immediately because frame synchronization on the TCP stream is lost.
 * - On return -1 (I/O error or mid-frame EOF truncation): close fd immediately.
 * - On return 0 (clean EOF prior to reading byte 0 of header): close fd cleanly.
 *
 * @param fd          Connected socket file descriptor.
 * @param hdr_out     Out-parameter receiving host-order header fields (non-NULL).
 * @param payload_buf Buffer to store incoming payload (may be NULL iff buf_cap == 0).
 * @param buf_cap     Maximum capacity of payload_buf in bytes.
 * @return  1 on success;
 *          0 on clean EOF before any header byte;
 *         -1 on I/O error or truncated header/payload;
 *         -2 on bad magic (hdr_out is populated);
 *         -3 on bad version (hdr_out is populated);
 *         -4 if hdr_out->payload_len > MAX_PAYLOAD_SIZE or > buf_cap (not drained).
 */
int recv_frame(int fd, frame_hdr_t *hdr_out, void *payload_buf, uint32_t buf_cap);

/**
 * @brief Send an ERROR (0xFF) frame with a UTF-8 diagnostic message.
 *
 * Sets header param = (uint32_t)code and sends msg (without NUL terminator)
 * as the payload. If msg is NULL, uses proto_err_name(code) as default text.
 *
 * @param fd   Connected socket file descriptor.
 * @param seq  Sequence number of the offending frame (or 0 if unknown).
 * @param code Protocol error code (proto_err_t).
 * @param msg  Optional UTF-8 error description string (may be NULL).
 * @return 0 on success; -1 on failure.
 */
int send_error(int fd, uint32_t seq, proto_err_t code, const char *msg);

/**
 * @brief Return a static human-readable name for a message type (for logging).
 * @param msg_type The 8-bit message type code.
 * @return Constant string such as "HELLO", "DATA", or "UNKNOWN_MSG".
 */
const char *proto_msg_name(uint8_t msg_type);

/**
 * @brief Return a static human-readable name for an error code (for logging).
 * @param code The 32-bit error code.
 * @return Constant string such as "BAD_MAGIC", "FILE_NOT_FOUND", or "UNKNOWN_ERR".
 */
const char *proto_err_name(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* PODFLOW_PROTOCOL_H */