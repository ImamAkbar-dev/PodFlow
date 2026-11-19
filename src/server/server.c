/*
 * server.c - PodFlow streaming server (owner: Member 1)
 *
 * Architecture (contract v1.1, frozen):
 *   accept loop (main thread)  -> one DETACHED "connection thread" per client.
 *   Each connection owns a stream_session_t (producer thread + 40-slot ring
 *   buffer, Member 2). The connection thread is the ONLY thread that writes to
 *   the client socket:
 *
 *     loop:
 *       ppoll(client_fd, timeout = time to next 50 ms tick if PLAYING, else 100 ms)
 *       if readable : recv_frame -> dispatch command -> ACK / ERROR
 *       if PLAYING and the tick is due : session_next -> send_frame -> pacer_tick_done
 *
 * Stale-DATA rule (why a command's ACK can never be followed by old DATA):
 *   1. This thread is the only sender, and it handles a command only between
 *      two pump steps, so no item is ever "popped but not yet sent" when a
 *      command is processed.
 *   2. session_seek()/session_stop() flush the ring buffer (epoch++) BEFORE they
 *      return; the ACK is written after that, so every later session_next()
 *      returns post-command data only.
 *   3. DATA that was already on the wire before the ACK is stale for the
 *      client; clients must ignore DATA until they have seen the ACK of their
 *      last command (contract rule).
 *
 * Blocking points of a connection thread (all bounded):
 *   - ppoll():  at most one tick (50 ms) while PLAYING, 100 ms otherwise.
 *   - recv_frame() after ppoll said "readable": readn() waits for the rest of a
 *     half-sent frame, at most SO_RCVTIMEO (RECV_TIMEOUT_SEC); then the client
 *     is dropped (a slow-loris client cannot stall anything but itself).
 *   - send_frame(): SO_SNDTIMEO (SEND_TIMEOUT_SEC); a client that stops reading
 *     is dropped once the kernel send buffer is full and the timeout expires.
 *
 * Client disconnect mid-stream: ppoll() reports POLLIN/POLLHUP, recv_frame()
 * returns 0 (or -1), or a send fails with EPIPE/ECONNRESET. The loop ends and
 * the thread runs one cleanup path: unregister, close(fd), session_destroy()
 * (stops + joins the producer, frees ring buffer, closes the WAV file), update
 * the global counters. No zombie threads: connection threads are detached.
 *
 * Shutdown (SIGINT/SIGTERM, handlers installed WITHOUT SA_RESTART): only the
 * main thread receives the signal; it stops accepting, shutdown()s every client
 * socket to wake the workers and waits up to SHUTDOWN_WAIT_SEC for them.
 *
 * usage: server [port] [--no-pacing] [--max-clients N] [--stats-csv path]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE            /* ppoll() */
#endif

#include "io_utils.h"
#include "pacing.h"
#include "protocol.h"
#include "stream_session.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MEDIA_DIR            "media"
#define LISTEN_BACKLOG       1024
#define WORKER_STACK_BYTES   (256 * 1024)   /* big buffers live on the heap */
#define DEFAULT_MAX_CLIENTS  1000
#define SEND_TIMEOUT_SEC     5              /* SO_SNDTIMEO */
#define RECV_TIMEOUT_SEC     2              /* SO_RCVTIMEO: half-sent frame limit */
#define IDLE_POLL_MS         100            /* ppoll timeout when not PLAYING */
#define STARVE_RETRY_NS      1000000L       /* 1 ms retry when the ring is empty */
#define SHUTDOWN_WAIT_SEC    5

/* ------------------------------------------------------------------ */
/* Configuration, global state, statistics                             */
/* ------------------------------------------------------------------ */

static struct {
    int         port;
    int         no_pacing;
    int         max_clients;
    const char *stats_path;
} g_cfg = { DEFAULT_PORT, 0, DEFAULT_MAX_CLIENTS, NULL };

static atomic_int g_stop;                   /* set by the signal handler */

/* Statistics: plain atomics, relaxed ordering, no lock on the hot path. */
static atomic_int       g_clients_now;      /* connected clients                 */
static atomic_int       g_streaming_now;    /* clients currently PLAYING         */
static atomic_ullong    g_chunks_sent;      /* PCM chunks sent (cumulative)      */
static atomic_ullong    g_underruns;        /* ticks missed: ring empty (cumul.) */
static atomic_ullong    g_late_resets;      /* pacer late-resets (cumulative)    */
static atomic_llong     g_max_late_ns;      /* max lateness since last CSV row   */
static atomic_ullong    g_rejected;         /* 503 SERVER_BUSY rejections        */

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct client_ctx {
    int                fd;
    uint32_t           id;
    char               peer[64];            /* "ip:port" */
    stream_session_t  *sess;
    pacer_t            pacer;
    int                playing;             /* mirrors SESS_PLAYING; only this thread changes it */
    int                primed;              /* >= 1 chunk sent since the last (re)start         */
    int                starved;             /* session_next() returned 0 for the pending tick   */
    int                underrun_counted;    /* count at most one underrun per tick              */
    uint64_t           late_resets_seen;
    unsigned long      frames_in;
    unsigned long      s_chunks;            /* current stream */
    unsigned long long s_bytes;
    unsigned long      s_underruns;
    unsigned long      t_chunks;            /* whole connection */
    unsigned long long t_bytes;
    session_item_t     item;                /* heap: 8.8 KB, keeps the stack small */
    struct client_ctx *prev, *next;         /* registry list */
} client_ctx_t;

static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_reg_cv = PTHREAD_COND_INITIALIZER;
static client_ctx_t   *g_clients = NULL;    /* guarded by g_reg_mu */
static int             g_active  = 0;       /* guarded by g_reg_mu */
static uint32_t        g_next_id = 0;       /* touched by the main thread only */

static inline int stop_requested(void)
{
    return atomic_load_explicit(&g_stop, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Logging: one line = one fprintf under a mutex (no interleaving)     */
/* ------------------------------------------------------------------ */

static void log_line(const char *level, const client_ctx_t *c,
                     const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void log_line(const char *level, const client_ctx_t *c,
                     const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tb[16];
    strftime(tb, sizeof tb, "%H:%M:%S", &tm);

    pthread_mutex_lock(&g_log_mu);
    if (c)
        fprintf(stdout, "%s.%03ld [%s] client#%u %s: %s\n", tb,
                ts.tv_nsec / 1000000, level, c->id, c->peer, msg);
    else
        fprintf(stdout, "%s.%03ld [%s] %s\n", tb, ts.tv_nsec / 1000000,
                level, msg);
    fflush(stdout);
    pthread_mutex_unlock(&g_log_mu);
}

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);  /* async-signal-safe */
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* NO SA_RESTART: blocking calls return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;         /* a vanished client must not kill us (EPIPE instead) */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

/* Threads created while this mask is active never receive SIGINT/SIGTERM. */
static void block_stop_signals(sigset_t *old)
{
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blk, old);
}

/* ------------------------------------------------------------------ */
/* Client registry (lets main wake up every worker at shutdown)        */
/* ------------------------------------------------------------------ */

static void registry_add(client_ctx_t *c)
{
    pthread_mutex_lock(&g_reg_mu);
    c->prev = NULL;
    c->next = g_clients;
    if (g_clients)
        g_clients->prev = c;
    g_clients = c;
    g_active++;
    pthread_mutex_unlock(&g_reg_mu);
}

static void registry_remove(client_ctx_t *c)
{
    pthread_mutex_lock(&g_reg_mu);
    if (c->prev)
        c->prev->next = c->next;
    else
        g_clients = c->next;
    if (c->next)
        c->next->prev = c->prev;
    pthread_mutex_unlock(&g_reg_mu);
}

static void registry_done(void)
{
    pthread_mutex_lock(&g_reg_mu);
    g_active--;
    pthread_cond_broadcast(&g_reg_cv);
    pthread_mutex_unlock(&g_reg_mu);
}

static void registry_shutdown_all(void)
{
    pthread_mutex_lock(&g_reg_mu);
    for (client_ctx_t *c = g_clients; c; c = c->next)
        shutdown(c->fd, SHUT_RDWR);   /* blocked ppoll/read/write returns */
    pthread_mutex_unlock(&g_reg_mu);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *err_text(proto_err_t code)
{
    switch (code) {
    case ERR_FILE_NOT_FOUND:    return "file not found";
    case ERR_INVALID_WAV:       return "invalid WAV file";
    case ERR_SEEK_OUT_OF_RANGE: return "chunk out of range";
    case ERR_INVALID_STATE:     return "command not valid in the current state";
    case ERR_INTERNAL_SERVER:   return "internal server error";
    default:                    return "error";
    }
}

/* Printable, bounded copy of a client-supplied string for log lines. */
static void sanitize(char *dst, size_t dst_cap, const uint8_t *src, size_t n)
{
    size_t m = n < dst_cap - 1 ? n : dst_cap - 1;
    for (size_t i = 0; i < m; i++)
        dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[m] = '\0';
}

/* Send one frame; log why it failed. Returns 0 on success, -1 = drop client. */
static int send_or_log(client_ctx_t *c, uint8_t type, uint32_t seq,
                       uint32_t param, const void *payload, uint32_t len)
{
    if (send_frame(c->fd, type, seq, param, payload, len) == 0)
        return 0;

    int e = errno;
    if (stop_requested())
        return -1;
    if (e == EAGAIN || e == EWOULDBLOCK)
        log_line("WARN", c, "send timeout (%d s): client stopped reading, dropping",
                 SEND_TIMEOUT_SEC);
    else if (e == EPIPE || e == ECONNRESET)
        log_line("INFO", c, "client went away mid-send (%s)", strerror(e));
    else
        log_line("WARN", c, "send failed: %s", strerror(e));
    return -1;
}

/* Send an ERROR frame. Returns 0 if sent, -1 if the connection is dead. */
static int reply_error(client_ctx_t *c, uint32_t seq, proto_err_t code,
                       const char *msg)
{
    log_line("WARN", c, "-> ERROR %d (%s)", (int)code, msg);
    return send_error(c->fd, seq, code, msg) == 0 ? 0 : -1;
}

/* After a protocol violation: let the client read our ERROR before closing. */
static void drain_briefly(int fd)
{
    struct timeval tv = {0, 200000};   /* 200 ms */
    char b[1024];
    size_t total = 0;
    ssize_t r;

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (total < 65536 && (r = read(fd, b, sizeof b)) > 0)
        total += (size_t)r;
}

static void update_max_late(long long late)
{
    long long cur = atomic_load_explicit(&g_max_late_ns, memory_order_relaxed);
    while (late > cur &&
           !atomic_compare_exchange_weak_explicit(&g_max_late_ns, &cur, late,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
        ;   /* cur was refreshed by the failed CAS */
}

static void set_playing(client_ctx_t *c, int on)
{
    if (on == c->playing)
        return;
    c->playing = on;
    atomic_fetch_add_explicit(&g_streaming_now, on ? 1 : -1, memory_order_relaxed);
}

/* New schedule point: PLAY(resume) / SEEK. First chunk is due immediately. */
static void stream_restart(client_ctx_t *c)
{
    pacer_reset(&c->pacer);
    c->primed = 0;
    c->starved = 0;
    c->underrun_counted = 0;
}

/* Brand-new stream (PLAY with a filename): fresh pacer and counters. */
static void stream_begin(client_ctx_t *c)
{
    pacer_init(&c->pacer, PACING_DEFAULT_INTERVAL_NS);
    c->late_resets_seen = 0;
    c->s_chunks = 0;
    c->s_bytes = 0;
    c->s_underruns = 0;
    stream_restart(c);
    set_playing(c, 1);
}

static void log_stream_summary(const client_ctx_t *c, const char *how)
{
    const pacer_t *p = &c->pacer;
    double avg_us = p->ticks ? (double)p->lateness_sum_ns / (double)p->ticks / 1e3 : 0.0;

    log_line("INFO", c,
             "stream %s: chunks=%lu bytes=%llu underruns=%lu late_resets=%llu "
             "lateness avg=%.0fus max=%.0fus",
             how, c->s_chunks, c->s_bytes, c->s_underruns,
             (unsigned long long)p->late_resets, avg_us,
             (double)p->lateness_max_ns / 1e3);
}

/* ------------------------------------------------------------------ */
/* Sender: one pacing tick                                             */
/* ------------------------------------------------------------------ */

/* Account a finished tick (lateness, late-resets) in the global statistics. */
static void tick_accounting(client_ctx_t *c)
{
    long long before = c->pacer.lateness_sum_ns;

    pacer_tick_done(&c->pacer);
    update_max_late(c->pacer.lateness_sum_ns - before);
    if (c->pacer.late_resets != c->late_resets_seen) {
        atomic_fetch_add_explicit(&g_late_resets,
                                  c->pacer.late_resets - c->late_resets_seen,
                                  memory_order_relaxed);
        c->late_resets_seen = c->pacer.late_resets;
    }
}

/*
 * Called when the tick is due (or always, with --no-pacing). Sends at most one
 * PCM chunk (plus the WAV header in front of chunk 1, and the END frame).
 *   - ITEM_HEADER does not consume a tick: chunk 1 follows at once.
 *   - ITEM_CHUNK consumes the tick (pacer_tick_done).
 *   - ring empty = underrun: counted once per tick, the tick is KEPT (no
 *     tick_done), the caller retries every STARVE_RETRY_NS. Nothing is skipped.
 * Returns 0 = keep connection, -1 = drop it.
 */
static int pump(client_ctx_t *c)
{
    for (;;) {
        session_item_t *it = &c->item;

        if (!session_next(c->sess, it)) {
            if (c->primed && !c->underrun_counted) {
                c->underrun_counted = 1;
                c->s_underruns++;
                atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
            }
            c->starved = 1;
            return 0;
        }
        c->starved = 0;

        switch (it->kind) {
        case ITEM_HEADER:
            if (send_or_log(c, MSG_DATA, it->seq, it->param, it->data, it->len) != 0)
                return -1;
            c->s_bytes += it->len;
            c->t_bytes += it->len;
            continue;                       /* chunk 1 in the same tick */

        case ITEM_CHUNK:
            if (send_or_log(c, MSG_DATA, it->seq, it->param, it->data, it->len) != 0)
                return -1;
            c->s_bytes += it->len;
            c->t_bytes += it->len;
            c->s_chunks++;
            c->t_chunks++;
            c->primed = 1;
            c->underrun_counted = 0;
            atomic_fetch_add_explicit(&g_chunks_sent, 1, memory_order_relaxed);
            if (!g_cfg.no_pacing)
                tick_accounting(c);
            return 0;

        case ITEM_END:
            if (send_or_log(c, MSG_END, it->seq, it->param, NULL, 0) != 0)
                return -1;
            set_playing(c, 0);              /* session is IDLE again */
            log_stream_summary(c, "finished");
            return 0;

        default:                            /* ITEM_NONE with rc 1: treat as empty */
            c->starved = 1;
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command handlers. Return 0 = keep connection, -1 = close it.        */
/* ------------------------------------------------------------------ */

static int handle_hello(client_ctx_t *c, const frame_hdr_t *h, const uint8_t *buf)
{
    char name[33];
    sanitize(name, sizeof name, buf, h->payload_len < 32 ? h->payload_len : 32);
    log_line("INFO", c, "HELLO name='%s'", name);
    return send_or_log(c, MSG_WELCOME, 0, PCM_CHUNK_SIZE, WELCOME_BANNER,
                       (uint32_t)strlen(WELCOME_BANNER));
}

/* LIST: names of *.wav in the media directory, '\n' separated, in an ACK frame. */
static int handle_list(client_ctx_t *c, const frame_hdr_t *h)
{
    DIR *d = opendir(MEDIA_DIR);
    if (!d)
        return reply_error(c, h->seq_num, ERR_INTERNAL_SERVER,
                           "cannot read media directory");

    char *out = malloc(MAX_PAYLOAD_SIZE);
    if (!out) {
        closedir(d);
        return reply_error(c, h->seq_num, ERR_INTERNAL_SERVER, "out of memory");
    }

    size_t len = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl <= 4 || strcmp(e->d_name + nl - 4, ".wav") != 0)
            continue;
        size_t need = nl + (len ? 1 : 0);
        if (len + need > MAX_PAYLOAD_SIZE)
            break;                       /* truncate list instead of overflowing */
        if (len)
            out[len++] = '\n';
        memcpy(out + len, e->d_name, nl);
        len += nl;
    }
    closedir(d);

    log_line("INFO", c, "LIST -> %zu bytes", len);
    int rc = send_or_log(c, MSG_ACK, h->seq_num, 0, out, (uint32_t)len);
    free(out);
    return rc;
}

static int handle_play(client_ctx_t *c, const frame_hdr_t *h, const uint8_t *buf)
{
    uint32_t ack = 0;
    uint32_t nlen = h->payload_len;
    proto_err_t e = session_play(c->sess, (const char *)buf, nlen, h->param, &ack);

    if (e != ERR_NONE)
        return reply_error(c, h->seq_num, e, err_text(e));
    if (send_or_log(c, MSG_ACK, h->seq_num, ack, NULL, 0) != 0)
        return -1;

    if (nlen > 0) {
        char name[65];
        sanitize(name, sizeof name, buf, nlen < 64 ? nlen : 64);
        log_line("INFO", c, "PLAY '%s' start_chunk=%u -> ACK %u", name, h->param, ack);
        stream_begin(c);
    } else {
        log_line("INFO", c, "PLAY (resume) -> ACK %u", ack);
        stream_restart(c);
        set_playing(c, 1);
    }
    return 0;
}

static int handle_pause(client_ctx_t *c, const frame_hdr_t *h)
{
    uint32_t ack = 0;
    proto_err_t e = session_pause(c->sess, &ack);

    if (e != ERR_NONE)
        return reply_error(c, h->seq_num, e, err_text(e));
    set_playing(c, 0);                  /* no DATA after the ACK: we are the only sender */
    log_line("INFO", c, "PAUSE -> ACK %u", ack);
    return send_or_log(c, MSG_ACK, h->seq_num, ack, NULL, 0);
}

static int handle_seek(client_ctx_t *c, const frame_hdr_t *h)
{
    uint32_t ack = 0;
    proto_err_t e = session_seek(c->sess, h->param, &ack);

    if (e != ERR_NONE)
        return reply_error(c, h->seq_num, e, err_text(e));
    log_line("INFO", c, "SEEK %u -> ACK %u", h->param, ack);
    if (send_or_log(c, MSG_ACK, h->seq_num, ack, NULL, 0) != 0)
        return -1;
    stream_restart(c);                  /* chunk 'target' goes out at once if PLAYING */
    return 0;
}

static int handle_stop(client_ctx_t *c, const frame_hdr_t *h)
{
    uint32_t ack = 0;
    proto_err_t e = session_stop(c->sess, &ack);

    if (e != ERR_NONE)
        return reply_error(c, h->seq_num, e, err_text(e));
    set_playing(c, 0);
    log_line("INFO", c, "STOP -> ACK %u", ack);
    log_stream_summary(c, "stopped");
    return send_or_log(c, MSG_ACK, h->seq_num, ack, NULL, 0);
}

static int dispatch(client_ctx_t *c, const frame_hdr_t *h, const uint8_t *buf)
{
    switch (h->msg_type) {
    case MSG_HELLO:
        return handle_hello(c, h, buf);
    case MSG_ECHO:                      /* any state */
        return send_or_log(c, MSG_ECHO, h->seq_num, h->param, buf, h->payload_len);
    case MSG_LIST:                      /* any state */
        return handle_list(c, h);
    case MSG_PLAY:
        return handle_play(c, h, buf);
    case MSG_PAUSE:
        return handle_pause(c, h);
    case MSG_SEEK:
        return handle_seek(c, h);
    case MSG_STOP:
        return handle_stop(c, h);
    default:
        return reply_error(c, h->seq_num, ERR_UNKNOWN_CMD, "unknown message type");
    }
}

/* ------------------------------------------------------------------ */
/* Connection thread: the ppoll loop                                   */
/* ------------------------------------------------------------------ */

/* Returns 0 = keep connection, -1 = close it. Handles one readable event. */
static int read_and_dispatch(client_ctx_t *c, uint8_t *rx)
{
    frame_hdr_t h;
    errno = 0;
    int rc = recv_frame(c->fd, &h, rx, MAX_PAYLOAD_SIZE);

    if (rc == 1) {
        c->frames_in++;
        return dispatch(c, &h, rx);
    }
    if (rc == 0) {
        log_line("INFO", c, "client disconnected");
    } else if (rc == -1) {
        if (!stop_requested()) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                log_line("WARN", c, "half-sent frame: no data for %d s, dropping",
                         RECV_TIMEOUT_SEC);
            else
                log_line("WARN", c, "I/O error or truncated frame%s%s",
                         errno ? ": " : "", errno ? strerror(errno) : "");
        }
    } else {
        /* protocol violation: ERROR, then close */
        proto_err_t code = ERR_PAYLOAD_TOO_LARGE;
        const char *msg = "payload too large";
        if (rc == -2) { code = ERR_BAD_MAGIC;   msg = "bad magic"; }
        if (rc == -3) { code = ERR_BAD_VERSION; msg = "bad version"; }
        log_line("WARN", c, "protocol violation (recv_frame=%d): %s", rc, msg);
        if (send_error(c->fd, 0, code, msg) == 0)
            drain_briefly(c->fd);
    }
    return -1;
}

static void connection_loop(client_ctx_t *c, uint8_t *rx)
{
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN, .revents = 0 };

    while (!stop_requested()) {
        struct timespec to;
        int due = 0;

        if (c->playing) {
            if (g_cfg.no_pacing) {
                to.tv_sec = 0;                     /* back-to-back, still reads commands */
                to.tv_nsec = c->starved ? STARVE_RETRY_NS : 0;
            } else {
                due = pacer_timeout(&c->pacer, &to);
                if (due && c->starved) {           /* ring was empty: do not spin */
                    to.tv_sec = 0;
                    to.tv_nsec = STARVE_RETRY_NS;
                }
            }
        } else {
            to.tv_sec = 0;
            to.tv_nsec = (long)IDLE_POLL_MS * 1000000L;
        }

        pfd.revents = 0;
        int pr = ppoll(&pfd, 1, &to, NULL);        /* ns timeout: poll() only has ms */
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            log_line("ERROR", c, "ppoll: %s", strerror(errno));
            return;
        }
        if (pr > 0 && read_and_dispatch(c, rx) != 0)
            return;

        if (c->playing) {
            struct timespec ignored;
            if ((g_cfg.no_pacing || pacer_timeout(&c->pacer, &ignored)) &&
                pump(c) != 0)
                return;
        }
    }
}

static void *client_thread(void *arg)
{
    client_ctx_t *c = arg;
    uint8_t *rx = malloc(MAX_PAYLOAD_SIZE);       /* heap: keeps the stack small */

    c->sess = session_create(MEDIA_DIR);
    if (!rx || !c->sess) {
        log_line("ERROR", c, "out of memory / session_create failed");
        send_error(c->fd, 0, ERR_INTERNAL_SERVER, "internal server error");
    } else {
        connection_loop(c, rx);
    }

    if (c->playing)
        log_stream_summary(c, "aborted (connection closed)");
    log_line("INFO", c, "closed (frames_in=%lu, chunks_sent=%lu, bytes_sent=%llu)",
             c->frames_in, c->t_chunks, c->t_bytes);

    registry_remove(c);        /* BEFORE close(): main must never shutdown() a stale fd */
    close(c->fd);
    set_playing(c, 0);
    session_destroy(c->sess);  /* stops + joins the producer; safe with NULL */
    free(rx);
    atomic_fetch_sub_explicit(&g_clients_now, 1, memory_order_relaxed);
    free(c);                   /* 'c' is invalid from here on */
    registry_done();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Accepting clients                                                   */
/* ------------------------------------------------------------------ */

/* Over --max-clients: ERROR 503 and close, without ever blocking the accept loop. */
static void reject_busy(int cfd)
{
    struct timeval tv = {1, 0};
    char sink[256];
    unsigned long long n = atomic_fetch_add_explicit(&g_rejected, 1,
                                                     memory_order_relaxed) + 1;

    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    send_error(cfd, 0, ERR_SERVER_BUSY, "server busy");
    shutdown(cfd, SHUT_WR);                       /* FIN after the ERROR frame */
    while (recv(cfd, sink, sizeof sink, MSG_DONTWAIT) > 0)
        ;                                         /* best effort: avoid an RST on close */
    close(cfd);

    if (n % 100 == 1)                             /* rate-limited log */
        log_line("WARN", NULL, "503 SERVER_BUSY (limit %d), %llu rejected so far",
                 g_cfg.max_clients, n);
}

static void spawn_client(int cfd, const struct sockaddr_in *ca)
{
    client_ctx_t *c = calloc(1, sizeof *c);
    if (!c) {
        log_line("ERROR", NULL, "calloc failed, dropping connection");
        close(cfd);
        return;
    }
    c->fd = cfd;
    c->id = ++g_next_id;

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ca->sin_addr, ip, sizeof ip);
    snprintf(c->peer, sizeof c->peer, "%s:%u", ip, (unsigned)ntohs(ca->sin_port));

    /* TCP_NODELAY: frames are small and time-critical; Nagle + delayed ACK
     * would add up to ~40 ms to a 50 ms cadence.
     * SO_SNDTIMEO: a client that stops reading is dropped once the send buffer
     * is full. SO_RCVTIMEO: bounds how long recv_frame waits for the rest of a
     * half-sent frame (ppoll already said "readable"). */
    int one = 1;
    struct timeval snd = { SEND_TIMEOUT_SEC, 0 };
    struct timeval rcv = { RECV_TIMEOUT_SEC, 0 };
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);

    int now = atomic_fetch_add_explicit(&g_clients_now, 1, memory_order_relaxed) + 1;
    log_line("INFO", c, "connected (clients=%d)", now);
    registry_add(c);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);

    sigset_t old;
    block_stop_signals(&old);       /* the worker inherits the mask */
    pthread_t tid;
    int err = pthread_create(&tid, &attr, client_thread, c);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        log_line("ERROR", c, "pthread_create failed: %s", strerror(err));
        registry_remove(c);
        registry_done();
        atomic_fetch_sub_explicit(&g_clients_now, 1, memory_order_relaxed);
        close(cfd);
        free(c);
    }
}

/* ------------------------------------------------------------------ */
/* Statistics writer: one CSV row per second                           */
/* ------------------------------------------------------------------ */

static void write_stats_row(FILE *f, long long t_s)
{
    long long max_late_ns = atomic_exchange_explicit(&g_max_late_ns, 0,
                                                     memory_order_relaxed);
    fprintf(f, "%lld,%d,%d,%llu,%llu,%llu,%lld\n", t_s,
            atomic_load_explicit(&g_clients_now, memory_order_relaxed),
            atomic_load_explicit(&g_streaming_now, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_chunks_sent, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_underruns, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_late_resets, memory_order_relaxed),
            max_late_ns / 1000);
    fflush(f);
}

static long long mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void *stats_thread(void *arg)
{
    FILE *f = arg;
    const long long t0 = mono_ns();
    long long row = 0;
    long long last_t_s = 0;

    fprintf(f, "t_s,clients,streaming,chunks_sent,underruns,late_resets,max_lateness_us\n");
    fflush(f);

    for (;;) {
        const long long deadline = t0 + (row + 1) * 1000000000LL;

        while (!stop_requested() && mono_ns() < deadline) {
            long long left = deadline - mono_ns();
            struct timespec nap = { 0, (long)(left < 100000000LL ? left : 100000000LL) };
            nanosleep(&nap, NULL);
        }
        row++;
        long long t_s = (mono_ns() - t0 + 500000000LL) / 1000000000LL;
        if (!stop_requested() || t_s > last_t_s)   /* final row only if it is a new second */
            write_stats_row(f, t_s);
        last_t_s = t_s;
        if (stop_requested())
            break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [port] [--no-pacing] [--max-clients N] [--stats-csv path]\n",
            prog);
}

static int parse_long(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno != 0 || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv)
{
    int have_port = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        long v;

        if (strcmp(a, "--no-pacing") == 0) {
            g_cfg.no_pacing = 1;
        } else if (strcmp(a, "--max-clients") == 0 && i + 1 < argc) {
            if (parse_long(argv[++i], 1, 1000000, &v) != 0) {
                fprintf(stderr, "invalid --max-clients: %s\n", argv[i]);
                return -1;
            }
            g_cfg.max_clients = (int)v;
        } else if (strcmp(a, "--stats-csv") == 0 && i + 1 < argc) {
            g_cfg.stats_path = argv[++i];
        } else if (a[0] != '-' && !have_port) {
            if (parse_long(a, 1, 65535, &v) != 0) {
                fprintf(stderr, "invalid port: %s\n", a);
                return -1;
            }
            g_cfg.port = (int)v;
            have_port = 1;
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0)
        return 2;

    install_signals();

    FILE *stats_file = NULL;
    pthread_t stats_tid;
    int stats_started = 0;

    if (g_cfg.stats_path) {
        stats_file = fopen(g_cfg.stats_path, "w");
        if (!stats_file) {
            fprintf(stderr, "cannot open %s: %s\n", g_cfg.stats_path, strerror(errno));
            return 1;
        }
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)g_cfg.port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(lfd);
        return 1;
    }
    if (listen(lfd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(lfd);
        return 1;
    }

    if (stats_file) {
        sigset_t old;
        block_stop_signals(&old);
        int err = pthread_create(&stats_tid, NULL, stats_thread, stats_file);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (err != 0) {
            fprintf(stderr, "cannot start stats thread: %s\n", strerror(err));
            close(lfd);
            return 1;
        }
        stats_started = 1;
    }

    log_line("INFO", NULL,
             "PodFlow server (%s) listening on port %d | media: ./%s | pacing: %s | "
             "max-clients: %d | stats: %s",
             WELCOME_BANNER, g_cfg.port, MEDIA_DIR, g_cfg.no_pacing ? "OFF" : "50 ms",
             g_cfg.max_clients, g_cfg.stats_path ? g_cfg.stats_path : "off");

    /* poll() with a timeout (instead of a bare accept()) so a signal arriving
     * just before the call can never make us sleep through the shutdown. */
    struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };

    while (!stop_requested()) {
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (pr == 0)
            continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                log_line("ERROR", NULL, "out of file descriptors (raise ulimit -n)");
                struct timespec nap = {0, 100 * 1000000L};
                nanosleep(&nap, NULL);
                continue;
            }
            perror("accept");
            break;
        }
        if (atomic_load_explicit(&g_clients_now, memory_order_relaxed) >= g_cfg.max_clients) {
            reject_busy(cfd);
            continue;
        }
        spawn_client(cfd, &ca);
    }

    /* ---- graceful shutdown ---- */
    log_line("INFO", NULL, "shutting down...");
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    close(lfd);
    registry_shutdown_all();

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SHUTDOWN_WAIT_SEC;

    pthread_mutex_lock(&g_reg_mu);
    while (g_active > 0) {
        if (pthread_cond_timedwait(&g_reg_cv, &g_reg_mu, &deadline) == ETIMEDOUT)
            break;
    }
    int left = g_active;
    pthread_mutex_unlock(&g_reg_mu);

    if (stats_started)
        pthread_join(stats_tid, NULL);
    if (stats_file)
        fclose(stats_file);

    if (left > 0)
        log_line("WARN", NULL, "%d worker(s) still running at exit", left);
    log_line("INFO", NULL,
             "totals: chunks_sent=%llu underruns=%llu late_resets=%llu rejected_503=%llu",
             (unsigned long long)atomic_load(&g_chunks_sent),
             (unsigned long long)atomic_load(&g_underruns),
             (unsigned long long)atomic_load(&g_late_resets),
             (unsigned long long)atomic_load(&g_rejected));
    log_line("INFO", NULL, "server stopped");
    return 0;
}