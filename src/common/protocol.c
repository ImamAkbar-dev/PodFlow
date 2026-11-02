/**
 * @file protocol.c
 * @brief Implementation of PodFlow framing, serialization, and error helpers.
 *
 * Owner: Member 2 (Protocol & Framing)
 */

#include "protocol.h"
#include "io_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

int send_frame(int fd, uint8_t msg_type, uint32_t seq, uint32_t param,
               const void *payload, uint32_t payload_len)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    /* Validate payload bounds BEFORE touching the stack buffer */
    if (payload_len > MAX_PAYLOAD_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (payload_len > 0 && payload == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Single thread-local stack buffer (16 + 65536 = 65,552 bytes).
     * Combining header + payload into one writen() call prevents splitting the
     * header and payload across two small write() syscalls, avoiding Nagle's
     * algorithm + TCP Delayed ACK 40ms stalls and ensuring atomic frame emission
     * per socket descriptor.
     */
    uint8_t frame_buf[FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE];

    frame_hdr_t wire_hdr;
    wire_hdr.magic       = htons((uint16_t)PROTO_MAGIC);
    wire_hdr.version     = (uint8_t)PROTO_VERSION;
    wire_hdr.msg_type    = msg_type;
    wire_hdr.seq_num     = htonl(seq);
    wire_hdr.payload_len = htonl(payload_len);
    wire_hdr.param       = htonl(param);

    memcpy(frame_buf, &wire_hdr, FRAME_HEADER_SIZE);

    if (payload_len > 0) {
        memcpy(frame_buf + FRAME_HEADER_SIZE, payload, (size_t)payload_len);
    }

    size_t total_len = (size_t)FRAME_HEADER_SIZE + (size_t)payload_len;
    ssize_t written = writen(fd, frame_buf, total_len);
    if (written < 0 || (size_t)written != total_len) {
        return -1;
    }

    return 0;
}

int recv_frame(int fd, frame_hdr_t *hdr_out, void *payload_buf, uint32_t buf_cap)
{
    if (fd < 0 || hdr_out == NULL) {
        errno = EINVAL;
        return RECV_ERR_IO;
    }
    if (buf_cap > 0 && payload_buf == NULL) {
        errno = EINVAL;
        return RECV_ERR_IO;
    }

    frame_hdr_t wire_hdr;
    memset(&wire_hdr, 0, sizeof(wire_hdr));

    ssize_t n_hdr = readn(fd, &wire_hdr, FRAME_HEADER_SIZE);
    if (n_hdr == 0) {
        /* Clean EOF before a single byte of the new frame header arrived */
        return RECV_EOF;
    }
    if (n_hdr < 0 || (size_t)n_hdr < FRAME_HEADER_SIZE) {
        /* Socket error or truncated header (1..15 bytes read before EOF) */
        return RECV_ERR_IO;
    }

    /* Convert multi-byte fields from big-endian wire order to host order */
    hdr_out->magic       = ntohs(wire_hdr.magic);
    hdr_out->version     = wire_hdr.version;
    hdr_out->msg_type    = wire_hdr.msg_type;
    hdr_out->seq_num     = ntohl(wire_hdr.seq_num);
    hdr_out->payload_len = ntohl(wire_hdr.payload_len);
    hdr_out->param       = ntohl(wire_hdr.param);

    /* Validate protocol invariants before attempting to read any payload */
    if (hdr_out->magic != PROTO_MAGIC) {
        return RECV_ERR_BAD_MAGIC;
    }
    if (hdr_out->version != PROTO_VERSION) {
        return RECV_ERR_BAD_VERSION;
    }
    if (hdr_out->payload_len > MAX_PAYLOAD_SIZE || hdr_out->payload_len > buf_cap) {
        /*
         * Do NOT drain the payload on -4: if a rogue peer sends payload_len = 2GB,
         * draining would block the server thread indefinitely. Caller must close fd.
         */
        return RECV_ERR_PAYLOAD_LEN;
    }

    if (hdr_out->payload_len > 0) {
        if (payload_buf == NULL) {
            errno = EINVAL;
            return RECV_ERR_IO;
        }
        ssize_t n_pay = readn(fd, payload_buf, (size_t)hdr_out->payload_len);
        if (n_pay < 0 || (uint32_t)n_pay != hdr_out->payload_len) {
            /* Mid-payload EOF truncation or socket error */
            return RECV_ERR_IO;
        }
    }

    return RECV_OK;
}

int send_error(int fd, uint32_t seq, proto_err_t code, const char *msg)
{
    const char *text = (msg != NULL) ? msg : proto_err_name((uint32_t)code);
    size_t len = strlen(text);
    if (len > MAX_PAYLOAD_SIZE) {
        len = MAX_PAYLOAD_SIZE;
    }

    return send_frame(fd, (uint8_t)ERROR, seq, (uint32_t)code,
                      (len > 0) ? text : NULL, (uint32_t)len);
}

const char *proto_msg_name(uint8_t msg_type)
{
    switch (msg_type) {
        case HELLO:   return "HELLO";
        case WELCOME: return "WELCOME";
        case LIST:    return "LIST";
        case PLAY:    return "PLAY";
        case PAUSE:   return "PAUSE";
        case SEEK:    return "SEEK";
        case STOP:    return "STOP";
        case DATA:    return "DATA";
        case END:     return "END";
        case ACK:     return "ACK";
        case ECHO:    return "ECHO";
        case ERROR:   return "ERROR";
        default:      return "UNKNOWN_MSG";
    }
}

const char *proto_err_name(uint32_t code)
{
    switch (code) {
        case NONE:              return "NONE";
        case BAD_MAGIC:         return "BAD_MAGIC";
        case BAD_VERSION:       return "BAD_VERSION";
        case UNKNOWN_CMD:       return "UNKNOWN_CMD";
        case PAYLOAD_TOO_LARGE: return "PAYLOAD_TOO_LARGE";
        case FILE_NOT_FOUND:    return "FILE_NOT_FOUND";
        case INVALID_WAV:       return "INVALID_WAV";
        case SEEK_OUT_OF_RANGE: return "SEEK_OUT_OF_RANGE";
        case INVALID_STATE:     return "INVALID_STATE";
        case INTERNAL_SERVER:   return "INTERNAL_SERVER";
        default:                return "UNKNOWN_ERR";
    }
}