/**
 * @file test_protocol.c
 * @brief Exhaustive unit, boundary, dribble, stress, and fuzz tests for protocol.c.
 *
 * Owner: Member 2 (Protocol & Framing)
 */

#define _POSIX_C_SOURCE 200809L

#include "protocol.h"
#include "io_utils.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Helper macro for clear test reporting */
#define RUN_TEST(fn)                                                \
    do {                                                            \
        printf("  [RUN ] %-45s ... ", #fn);                          \
        fflush(stdout);                                             \
        fn();                                                       \
        printf("PASS\n");                                           \
    } while (0)

static void make_socketpair(int sv[2])
{
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);
}

/* ========================================================================== *
 * TEST 1: Round-trip every defined msg_type + name helper verification       *
 * ========================================================================== */
static void test_all_message_types_and_names(void)
{
    const uint8_t types[] = {
        HELLO, WELCOME, LIST, PLAY, PAUSE, SEEK,
        STOP, DATA, END, ACK, ECHO, ERROR
    };
    const size_t num_types = sizeof(types) / sizeof(types[0]);

    int sv[2];
    make_socketpair(sv);

    for (size_t i = 0; i < num_types; i++) {
        uint8_t t = types[i];
        uint32_t seq = (uint32_t)(i + 100);
        uint32_t param = (uint32_t)(i * 42);
        const char *msg = proto_msg_name(t);
        assert(strcmp(msg, "UNKNOWN_MSG") != 0);

        uint32_t plen = (uint32_t)strlen(msg);
        assert(send_frame(sv[0], t, seq, param, msg, plen) == 0);

        frame_hdr_t hdr;
        char buf[128];
        memset(buf, 0, sizeof(buf));
        int rc = recv_frame(sv[1], &hdr, buf, sizeof(buf));
        assert(rc == RECV_OK);
        assert(hdr.magic == PROTO_MAGIC);
        assert(hdr.version == PROTO_VERSION);
        assert(hdr.msg_type == t);
        assert(hdr.seq_num == seq);
        assert(hdr.param == param);
        assert(hdr.payload_len == plen);
        assert(memcmp(buf, msg, plen) == 0);
    }

    /* Verify unknown message and error code name helpers */
    assert(strcmp(proto_msg_name(0x99), "UNKNOWN_MSG") == 0);
    assert(strcmp(proto_err_name(BAD_MAGIC), "BAD_MAGIC") == 0);
    assert(strcmp(proto_err_name(FILE_NOT_FOUND), "FILE_NOT_FOUND") == 0);
    assert(strcmp(proto_err_name(9999), "UNKNOWN_ERR") == 0);

    /* Also verify send_error() with explicit message and NULL message */
    assert(send_error(sv[0], 77, FILE_NOT_FOUND, "missing sample.wav") == 0);
    frame_hdr_t err_hdr;
    char err_buf[128];
    assert(recv_frame(sv[1], &err_hdr, err_buf, sizeof(err_buf)) == RECV_OK);
    assert(err_hdr.msg_type == ERROR);
    assert(err_hdr.seq_num == 77);
    assert(err_hdr.param == FILE_NOT_FOUND);
    assert(err_hdr.payload_len == strlen("missing sample.wav"));
    assert(memcmp(err_buf, "missing sample.wav", err_hdr.payload_len) == 0);

    assert(send_error(sv[0], 78, INVALID_STATE, NULL) == 0);
    assert(recv_frame(sv[1], &err_hdr, err_buf, sizeof(err_buf)) == RECV_OK);
    assert(err_hdr.msg_type == ERROR);
    assert(err_hdr.param == INVALID_STATE);
    assert(err_hdr.payload_len == strlen("INVALID_STATE"));
    assert(memcmp(err_buf, "INVALID_STATE", err_hdr.payload_len) == 0);

    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================== *
 * TEST 2: Boundary payload sizes (0, 1, 15, 16, 17, 8820, 65536)             *
 *         Uses a reader thread for 65536 so AF_UNIX buffer never deadlocks.  *
 * ========================================================================== */
typedef struct {
    int fd;
    uint32_t expected_len;
    uint32_t expected_seq;
    uint8_t *expected_data;
} boundary_arg_t;

static void *boundary_reader_thread(void *arg)
{
    boundary_arg_t *ba = (boundary_arg_t *)arg;
    uint8_t *rx_buf = (uint8_t *)malloc(MAX_PAYLOAD_SIZE);
    assert(rx_buf != NULL);

    frame_hdr_t hdr;
    int rc = recv_frame(ba->fd, &hdr, rx_buf, MAX_PAYLOAD_SIZE);
    assert(rc == RECV_OK);
    assert(hdr.msg_type == DATA);
    assert(hdr.seq_num == ba->expected_seq);
    assert(hdr.payload_len == ba->expected_len);
    if (ba->expected_len > 0) {
        assert(memcmp(rx_buf, ba->expected_data, ba->expected_len) == 0);
    }

    free(rx_buf);
    return NULL;
}

static void test_payload_size_boundaries(void)
{
    const uint32_t sizes[] = { 0, 1, 15, 16, 17, 8820, 65536 };
    const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    uint8_t *tx_buf = (uint8_t *)malloc(MAX_PAYLOAD_SIZE);
    assert(tx_buf != NULL);
    for (uint32_t i = 0; i < MAX_PAYLOAD_SIZE; i++) {
        tx_buf[i] = (uint8_t)((i * 31U + 7U) & 0xFFU);
    }

    for (size_t i = 0; i < num_sizes; i++) {
        int sv[2];
        make_socketpair(sv);

        boundary_arg_t ba = {
            .fd = sv[1],
            .expected_len = sizes[i],
            .expected_seq = (uint32_t)i,
            .expected_data = tx_buf
        };

        pthread_t tid;
        int prc = pthread_create(&tid, NULL, boundary_reader_thread, &ba);
        assert(prc == 0);

        const void *p = (sizes[i] == 0) ? NULL : tx_buf;
        assert(send_frame(sv[0], DATA, (uint32_t)i, 0, p, sizes[i]) == 0);

        pthread_join(tid, NULL);
        close(sv[0]);
        close(sv[1]);
    }

    free(tx_buf);
}

/* ========================================================================== *
 * TEST 3: Reject invalid send_frame arguments (65537 bytes, NULL payload)    *
 * ========================================================================== */
static void test_send_frame_rejections(void)
{
    int sv[2];
    make_socketpair(sv);

    uint8_t dummy[16] = {0};

    /* 65537 (> MAX_PAYLOAD_SIZE) must be rejected before touching buffer */
    assert(send_frame(sv[0], DATA, 1, 0, dummy, 65537U) == -1);
    assert(errno == EMSGSIZE);

    /* NULL payload with payload_len > 0 must be rejected */
    assert(send_frame(sv[0], DATA, 1, 0, NULL, 10U) == -1);
    assert(errno == EINVAL);

    /* Negative fd must be rejected */
    assert(send_frame(-1, HELLO, 0, 0, NULL, 0) == -1);
    assert(errno == EBADF);

    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================== *
 * TEST 4: Bad magic (-2), bad version (-3), oversize payload_len (-4),       *
 *         and payload_len > buf_cap (-4) without buffer overflow             *
 * ========================================================================== */
static void test_header_validation_errors(void)
{
    /* 4a. Bad magic -> -2 */
    {
        int sv[2];
        make_socketpair(sv);
        frame_hdr_t raw = {
            .magic = htons(0x4242), /* Wrong magic ("BB") */
            .version = PROTO_VERSION,
            .msg_type = HELLO,
            .seq_num = htonl(12),
            .payload_len = htonl(0),
            .param = htonl(0)
        };
        assert(writen(sv[0], &raw, sizeof(raw)) == (ssize_t)sizeof(raw));

        frame_hdr_t out;
        uint8_t buf[64];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_ERR_BAD_MAGIC);
        assert(out.magic == 0x4242);
        assert(out.seq_num == 12);
        close(sv[0]);
        close(sv[1]);
    }

    /* 4b. Bad version -> -3 */
    {
        int sv[2];
        make_socketpair(sv);
        frame_hdr_t raw = {
            .magic = htons(PROTO_MAGIC),
            .version = 99, /* Unsupported version */
            .msg_type = HELLO,
            .seq_num = htonl(34),
            .payload_len = htonl(0),
            .param = htonl(0)
        };
        assert(writen(sv[0], &raw, sizeof(raw)) == (ssize_t)sizeof(raw));

        frame_hdr_t out;
        uint8_t buf[64];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_ERR_BAD_VERSION);
        assert(out.version == 99);
        assert(out.seq_num == 34);
        close(sv[0]);
        close(sv[1]);
    }

    /* 4c. Oversize payload_len (65537 > MAX_PAYLOAD_SIZE) -> -4 without draining */
    {
        int sv[2];
        make_socketpair(sv);
        frame_hdr_t raw = {
            .magic = htons(PROTO_MAGIC),
            .version = PROTO_VERSION,
            .msg_type = DATA,
            .seq_num = htonl(56),
            .payload_len = htonl(65537U),
            .param = htonl(0)
        };
        /* Notice we ONLY write the 16-byte header and do NOT write 65537 payload
         * bytes. Because recv_frame must return -4 WITHOUT draining, it returns
         * immediately instead of blocking on readn()! */
        assert(writen(sv[0], &raw, sizeof(raw)) == (ssize_t)sizeof(raw));

        frame_hdr_t out;
        uint8_t buf[MAX_PAYLOAD_SIZE];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_ERR_PAYLOAD_LEN);
        assert(out.payload_len == 65537U);
        close(sv[0]);
        close(sv[1]);
    }

    /* 4d. payload_len > buf_cap -> -4 and canary bytes untouched */
    {
        int sv[2];
        make_socketpair(sv);
        uint8_t payload[64];
        memset(payload, 0xAA, sizeof(payload));
        assert(send_frame(sv[0], ECHO, 9, 0, payload, 64) == 0);

        frame_hdr_t out;
        uint8_t guarded_buf[32];
        memset(guarded_buf, 0x55, sizeof(guarded_buf));

        /* Pass buf_cap = 16 while incoming frame has payload_len = 64 */
        assert(recv_frame(sv[1], &out, guarded_buf, 16) == RECV_ERR_PAYLOAD_LEN);
        assert(out.payload_len == 64);

        /* Verify not a single byte of guarded_buf was overwritten */
        for (size_t i = 0; i < sizeof(guarded_buf); i++) {
            assert(guarded_buf[i] == 0x55);
        }
        close(sv[0]);
        close(sv[1]);
    }
}

/* ========================================================================== *
 * TEST 5: Clean EOF (0), Truncated Header (-1), and Truncated Payload (-1)   *
 * ========================================================================== */
static void test_eof_and_truncation(void)
{
    /* 5a. Clean EOF before any header byte -> returns 0 */
    {
        int sv[2];
        make_socketpair(sv);
        close(sv[0]); /* Close writer immediately */

        frame_hdr_t out;
        uint8_t buf[32];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_EOF);
        close(sv[1]);
    }

    /* 5b. Truncated header (only 7 of 16 bytes sent before close) -> returns -1 */
    {
        int sv[2];
        make_socketpair(sv);
        uint8_t partial_hdr[7] = { 0x57, 0x53, 0x01, 0x01, 0x00, 0x00, 0x00 };
        assert(writen(sv[0], partial_hdr, sizeof(partial_hdr)) == 7);
        close(sv[0]);

        frame_hdr_t out;
        uint8_t buf[32];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_ERR_IO);
        close(sv[1]);
    }

    /* 5c. Truncated payload (header claims 100 bytes, only 40 sent) -> returns -1 */
    {
        int sv[2];
        make_socketpair(sv);
        frame_hdr_t raw = {
            .magic = htons(PROTO_MAGIC),
            .version = PROTO_VERSION,
            .msg_type = DATA,
            .seq_num = htonl(1),
            .payload_len = htonl(100),
            .param = htonl(0)
        };
        uint8_t partial_payload[40];
        memset(partial_payload, 0x7F, sizeof(partial_payload));

        assert(writen(sv[0], &raw, sizeof(raw)) == (ssize_t)sizeof(raw));
        assert(writen(sv[0], partial_payload, sizeof(partial_payload)) == 40);
        close(sv[0]);

        frame_hdr_t out;
        uint8_t buf[128];
        assert(recv_frame(sv[1], &out, buf, sizeof(buf)) == RECV_ERR_IO);
        close(sv[1]);
    }
}

/* ========================================================================== *
 * TEST 6: "Dribble" test (writer sends frame 1 byte at a time)               *
 *         Proves readn() inside recv_frame() reassembles short reads.        *
 * ========================================================================== */
typedef struct {
    int fd;
    const uint8_t *raw_frame;
    size_t total_bytes;
} dribble_arg_t;

static void *dribble_writer_thread(void *arg)
{
    dribble_arg_t *da = (dribble_arg_t *)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L }; /* 100 microseconds */

    for (size_t i = 0; i < da->total_bytes; i++) {
        ssize_t w = write(da->fd, &da->raw_frame[i], 1);
        assert(w == 1);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_dribble_single_byte_reads(void)
{
    int sv[2];
    make_socketpair(sv);

    const char *payload = "Dribble-test payload proving short-read reassembly!";
    uint32_t plen = (uint32_t)strlen(payload);
    uint8_t raw_frame[FRAME_HEADER_SIZE + 128];

    frame_hdr_t wire_hdr = {
        .magic = htons(PROTO_MAGIC),
        .version = PROTO_VERSION,
        .msg_type = ECHO,
        .seq_num = htonl(4242),
        .payload_len = htonl(plen),
        .param = htonl(9001)
    };
    memcpy(raw_frame, &wire_hdr, FRAME_HEADER_SIZE);
    memcpy(raw_frame + FRAME_HEADER_SIZE, payload, plen);

    dribble_arg_t da = {
        .fd = sv[0],
        .raw_frame = raw_frame,
        .total_bytes = FRAME_HEADER_SIZE + plen
    };

    pthread_t tid;
    assert(pthread_create(&tid, NULL, dribble_writer_thread, &da) == 0);

    frame_hdr_t hdr_out;
    char rx_buf[128];
    memset(rx_buf, 0, sizeof(rx_buf));

    int rc = recv_frame(sv[1], &hdr_out, rx_buf, sizeof(rx_buf));
    assert(rc == RECV_OK);
    assert(hdr_out.msg_type == ECHO);
    assert(hdr_out.seq_num == 4242);
    assert(hdr_out.param == 9001);
    assert(hdr_out.payload_len == plen);
    assert(memcmp(rx_buf, payload, plen) == 0);

    pthread_join(tid, NULL);
    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================== *
 * TEST 7: 1,000-Frame Continuous Stream Round-Trip                           *
 * ========================================================================== */
#define STREAM_FRAME_COUNT 1000U

static void *stream_sender_thread(void *arg)
{
    int fd = *(int *)arg;
    uint8_t buf[PCM_CHUNK_SIZE];

    for (uint32_t i = 0; i < STREAM_FRAME_COUNT; i++) {
        uint32_t len = (i * 73U) % (PCM_CHUNK_SIZE + 1U);
        for (uint32_t j = 0; j < len; j++) {
            buf[j] = (uint8_t)((i + j) & 0xFFU);
        }
        int rc = send_frame(fd, DATA, i, i * 2U, (len > 0) ? buf : NULL, len);
        assert(rc == 0);
    }
    return NULL;
}

static void test_1000_frame_round_trip(void)
{
    int sv[2];
    make_socketpair(sv);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, stream_sender_thread, &sv[0]) == 0);

    uint8_t rx_buf[PCM_CHUNK_SIZE];
    for (uint32_t i = 0; i < STREAM_FRAME_COUNT; i++) {
        frame_hdr_t hdr;
        int rc = recv_frame(sv[1], &hdr, rx_buf, sizeof(rx_buf));
        assert(rc == RECV_OK);
        assert(hdr.msg_type == DATA);
        assert(hdr.seq_num == i);
        assert(hdr.param == i * 2U);

        uint32_t expected_len = (i * 73U) % (PCM_CHUNK_SIZE + 1U);
        assert(hdr.payload_len == expected_len);
        for (uint32_t j = 0; j < expected_len; j++) {
            assert(rx_buf[j] == (uint8_t)((i + j) & 0xFFU));
        }
    }

    pthread_join(tid, NULL);
    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================== *
 * TEST 8: Random-Garbage Fuzz Loop (Must Never Crash or Overflow Buffer)     *
 * ========================================================================== */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_random_garbage_fuzz_loop(void)
{
    uint32_t rng = 0xC0FFEE11U;
    const int iterations = 500;

    /* Allocate small receive buffer surrounded by 64-byte guard canaries */
    const uint32_t cap = 256;
    uint8_t arena[64 + 256 + 64];
    memset(arena, 0xCC, sizeof(arena));
    uint8_t *rx_buf = arena + 64;

    for (int iter = 0; iter < iterations; iter++) {
        int sv[2];
        make_socketpair(sv);

        uint8_t garbage[512];
        size_t glen = (size_t)(xorshift32(&rng) % sizeof(garbage));
        for (size_t b = 0; b < glen; b++) {
            garbage[b] = (uint8_t)(xorshift32(&rng) & 0xFFU);
        }

        /* Every 5th iteration, force valid magic + version so fuzzer also
         * hits the payload_len & payload reading code paths! */
        if (iter % 5 == 0 && glen >= FRAME_HEADER_SIZE) {
            garbage[0] = 0x57;
            garbage[1] = 0x53;
            garbage[2] = PROTO_VERSION;
        }

        if (glen > 0) {
            ssize_t w = writen(sv[0], garbage, glen);
            assert(w == (ssize_t)glen);
        }
        close(sv[0]);

        frame_hdr_t hdr;
        int rc = recv_frame(sv[1], &hdr, rx_buf, cap);
        assert(rc == RECV_OK ||
               rc == RECV_EOF ||
               rc == RECV_ERR_IO ||
               rc == RECV_ERR_BAD_MAGIC ||
               rc == RECV_ERR_BAD_VERSION ||
               rc == RECV_ERR_PAYLOAD_LEN);

        /* Verify pre-canary and post-canary are completely intact */
        for (size_t c = 0; c < 64; c++) {
            assert(arena[c] == 0xCC);
            assert(arena[64 + cap + c] == 0xCC);
        }

        close(sv[1]);
    }
}

int main(void)
{
    /* Ignore SIGPIPE so writing to a closed socket returns -1/EPIPE cleanly */
    signal(SIGPIPE, SIG_IGN);

    printf("====================================================================\n");
    printf("PodFlow Session 1 — Protocol & Framing Unit Test Suite (Member 2)\n");
    printf("====================================================================\n");

    RUN_TEST(test_all_message_types_and_names);
    RUN_TEST(test_payload_size_boundaries);
    RUN_TEST(test_send_frame_rejections);
    RUN_TEST(test_header_validation_errors);
    RUN_TEST(test_eof_and_truncation);
    RUN_TEST(test_dribble_single_byte_reads);
    RUN_TEST(test_1000_frame_round_trip);
    RUN_TEST(test_random_garbage_fuzz_loop);

    printf("====================================================================\n");
    printf("ALL 8 PROTOCOL TEST SUITES PASSED SUCCESSFULLY.\n");
    printf("====================================================================\n");
    return 0;
}