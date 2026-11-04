/*
 * server.c - wav-streamer / PodFlow Session 1 (owner: Member 1)
 *
 * Thread-per-client TCP server: HELLO/WELCOME, ECHO, LIST, PLAY (no pacing).
 * Graceful shutdown on SIGINT/SIGTERM.
 */
#include "protocol.h"
#include "io_utils.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MEDIA_DIR          "media"
#define SERVER_BANNER      "wav-streamer/1"
#define LISTEN_BACKLOG     128
#define WORKER_STACK_BYTES (256 * 1024)  /* big buffers live on the heap */
#define MAX_NAME_LEN       255
#define SHUTDOWN_WAIT_SEC  5

/* ------------------------------------------------------------------ */
/* Types and global state                                              */
/* ------------------------------------------------------------------ */

typedef struct client_ctx {
    int                fd;
    uint32_t           id;
    char               peer[64];       /* "ip:port" */
    unsigned long long bytes_sent;     /* WAV bytes streamed (incl. header) */
    unsigned long      chunks_sent;    /* PCM chunks streamed */
    unsigned long      frames_in;      /* frames received from the client */
    struct client_ctx *prev, *next;    /* registry list */
} client_ctx_t;

static volatile sig_atomic_t g_stop = 0;

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_reg_cv = PTHREAD_COND_INITIALIZER;
static client_ctx_t   *g_clients = NULL;   /* guarded by g_reg_mu */
static int             g_active  = 0;      /* guarded by g_reg_mu */

static uint32_t g_next_id = 0;             /* touched by the main thread only */

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
    g_stop = 1;   /* only async-signal-safe work here */
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
    sa.sa_handler = SIG_IGN;         /* a vanished client must not kill us */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
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
        shutdown(c->fd, SHUT_RDWR);   /* blocked read() returns 0 */
    pthread_mutex_unlock(&g_reg_mu);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Send an ERROR frame. Returns 0 if sent, -1 if the connection is dead. */
static int reply_error(client_ctx_t *c, uint32_t seq, proto_err_t code,
                       const char *msg)
{
    log_line("WARN", c, "-> ERROR %d (%s)", (int)code, msg);
    return send_error(c->fd, seq, code, msg) == 0 ? 0 : -1;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Canonical 44-byte PCM/16-bit/44100 Hz/stereo header. Returns 1 if valid. */
static int wav_header_ok(const uint8_t *h)
{
    return memcmp(h, "RIFF", 4) == 0 &&
           memcmp(h + 8, "WAVE", 4) == 0 &&
           memcmp(h + 12, "fmt ", 4) == 0 &&
           le32(h + 16) == 16 &&
           le16(h + 20) == 1 &&        /* PCM */
           le16(h + 22) == 2 &&        /* stereo */
           le32(h + 24) == 44100 &&
           le16(h + 34) == 16 &&
           memcmp(h + 36, "data", 4) == 0;
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

/* ------------------------------------------------------------------ */
/* Command handlers. Return 0 = keep connection, -1 = close it.        */
/* ------------------------------------------------------------------ */

static int handle_hello(client_ctx_t *c, const frame_hdr_t *h,
                        const uint8_t *buf)
{
    char name[33];
    size_t n = h->payload_len < 32 ? h->payload_len : 32;
    for (size_t i = 0; i < n; i++)
        name[i] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '?';
    name[n] = '\0';

    log_line("INFO", c, "HELLO name='%s'", name);
    return send_frame(c->fd, MSG_WELCOME, 0, PCM_CHUNK_SIZE, SERVER_BANNER,
                      (uint32_t)strlen(SERVER_BANNER)) == 0 ? 0 : -1;
}

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
    int rc = send_frame(c->fd, MSG_ACK, h->seq_num, 0, out, (uint32_t)len);
    free(out);
    return rc == 0 ? 0 : -1;
}

static int stream_wav_file(client_ctx_t *c, uint32_t req_seq, const char *name)
{
    char path[512];
    int pl = snprintf(path, sizeof path, "%s/%s", MEDIA_DIR, name);
    if (pl < 0 || (size_t)pl >= sizeof path)
        return reply_error(c, req_seq, ERR_FILE_NOT_FOUND, "file not found");

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return reply_error(c, req_seq, ERR_FILE_NOT_FOUND, "file not found");

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return reply_error(c, req_seq, ERR_FILE_NOT_FOUND, "file not found");
    }

    uint8_t wav[WAV_HEADER_SIZE];
    if (readn(fd, wav, WAV_HEADER_SIZE) != WAV_HEADER_SIZE ||
        !wav_header_ok(wav)) {
        close(fd);
        return reply_error(c, req_seq, ERR_INVALID_WAV,
                           "invalid or unsupported WAV header");
    }

    uint8_t *chunk = malloc(PCM_CHUNK_SIZE);
    if (!chunk) {
        close(fd);
        return reply_error(c, req_seq, ERR_INTERNAL_SERVER, "out of memory");
    }

    int rc = -1;
    uint32_t seq = 0;
    unsigned long long total = 0;
    unsigned long chunks = 0;

    log_line("INFO", c, "PLAY %s (%lld bytes)", name, (long long)st.st_size);

    /* seq 0: the 44-byte WAV header */
    if (send_frame(c->fd, MSG_DATA, seq++, 0, wav, WAV_HEADER_SIZE) != 0)
        goto out;
    total += WAV_HEADER_SIZE;

    /* seq 1..N: raw PCM, 8820 bytes each (the last one may be shorter) */
    for (;;) {
        if (g_stop)
            goto out;
        ssize_t n = readn(fd, chunk, PCM_CHUNK_SIZE);
        if (n < 0) {
            reply_error(c, req_seq, ERR_INTERNAL_SERVER, "file read error");
            goto out;
        }
        if (n == 0)
            break;                       /* file size was an exact multiple */
        if (send_frame(c->fd, MSG_DATA, seq++, 0, chunk, (uint32_t)n) != 0)
            goto out;
        total += (unsigned long long)n;
        chunks++;
        if (n < PCM_CHUNK_SIZE)
            break;                       /* short chunk = EOF */
    }

    /* END: seq = N+1, param = total bytes sent including the header */
    if (send_frame(c->fd, MSG_END, seq, (uint32_t)total, NULL, 0) != 0)
        goto out;

    c->bytes_sent += total;
    c->chunks_sent += chunks;
    log_line("INFO", c, "PLAY done: %s, 1 header + %lu chunks, %llu bytes",
             name, chunks, total);
    rc = 0;

out:
    if (rc != 0)
        log_line("WARN", c, "stream aborted after %llu bytes (%lu chunks)",
                 total, chunks);
    free(chunk);
    close(fd);
    return rc;
}

static int handle_play(client_ctx_t *c, const frame_hdr_t *h,
                       const uint8_t *buf)
{
    char name[MAX_NAME_LEN + 1];
    uint32_t len = h->payload_len;

    if (len == 0 || len > MAX_NAME_LEN || memchr(buf, '\0', len) != NULL)
        return reply_error(c, h->seq_num, ERR_FILE_NOT_FOUND,
                           "invalid file name");
    memcpy(name, buf, len);
    name[len] = '\0';

    /* path traversal guard */
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        log_line("WARN", c, "rejected suspicious file name");
        return reply_error(c, h->seq_num, ERR_FILE_NOT_FOUND,
                           "invalid file name");
    }
    return stream_wav_file(c, h->seq_num, name);
}

static int dispatch(client_ctx_t *c, const frame_hdr_t *h, const uint8_t *buf)
{
    switch (h->msg_type) {
    case MSG_HELLO:
        return handle_hello(c, h, buf);
    case MSG_ECHO:
        return send_frame(c->fd, MSG_ECHO, h->seq_num, h->param, buf,
                          h->payload_len) == 0 ? 0 : -1;
    case MSG_LIST:
        return handle_list(c, h);
    case MSG_PLAY:
        return handle_play(c, h, buf);
    case MSG_PAUSE:
    case MSG_SEEK:
    case MSG_STOP:
        return reply_error(c, h->seq_num, ERR_INVALID_STATE,
                           "command not available in session 1");
    default:
        return reply_error(c, h->seq_num, ERR_UNKNOWN_CMD,
                           "unknown message type");
    }
}

/* ------------------------------------------------------------------ */
/* Worker thread (one per client)                                      */
/* ------------------------------------------------------------------ */

static void *client_thread(void *arg)
{
    client_ctx_t *c = arg;
    uint8_t *rx = malloc(MAX_PAYLOAD_SIZE);   /* heap: keeps the stack small */

    if (!rx) {
        log_line("ERROR", c, "out of memory");
    } else {
        for (;;) {
            frame_hdr_t h;
            int rc = recv_frame(c->fd, &h, rx, MAX_PAYLOAD_SIZE);

            if (rc == 1) {
                c->frames_in++;
                if (dispatch(c, &h, rx) != 0)
                    break;
                continue;
            }
            if (rc == 0) {
                log_line("INFO", c, "client disconnected");
            } else if (rc == -1) {
                if (!g_stop)
                    log_line("WARN", c, "I/O error or truncated frame");
            } else {
                /* protocol violation: ERROR, then close */
                proto_err_t code = ERR_PAYLOAD_TOO_LARGE;
                const char *msg = "payload too large";
                if (rc == -2) { code = ERR_BAD_MAGIC;   msg = "bad magic"; }
                if (rc == -3) { code = ERR_BAD_VERSION; msg = "bad version"; }
                log_line("WARN", c, "protocol violation (recv_frame=%d): %s",
                         rc, msg);
                if (send_error(c->fd, 0, code, msg) == 0)
                    drain_briefly(c->fd);
            }
            break;
        }
    }

    log_line("INFO", c, "closed (frames_in=%lu, chunks_sent=%lu, bytes_sent=%llu)",
             c->frames_in, c->chunks_sent, c->bytes_sent);

    registry_remove(c);   /* BEFORE close(): main must never shutdown() a stale fd */
    close(c->fd);
    free(rx);
    free(c);              /* 'c' is invalid from here on */
    registry_done();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Accepting clients                                                   */
/* ------------------------------------------------------------------ */

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

    /* send_frame() does two writes (header, payload): without TCP_NODELAY,
     * Nagle + delayed ACK can add ~40 ms per small frame. */
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    log_line("INFO", c, "connected");
    registry_add(c);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);

    /* Workers must not receive SIGINT/SIGTERM: block them around create so the
     * new thread inherits the mask; only the main thread handles shutdown. */
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blk, &old);

    pthread_t tid;
    int err = pthread_create(&tid, &attr, client_thread, c);

    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        log_line("ERROR", c, "pthread_create failed: %s", strerror(err));
        registry_remove(c);
        registry_done();
        close(cfd);
        free(c);
    }
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || v < 1 || v > 65535) {
            fprintf(stderr, "invalid port: %s\n", argv[1]);
            return 2;
        }
        port = (int)v;
    }

    install_signals();

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
    addr.sin_port = htons((uint16_t)port);

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
    log_line("INFO", NULL, "server listening on port %d (media dir: ./%s)",
             port, MEDIA_DIR);

    /* poll() with a timeout (instead of a bare accept()) so a signal arriving
     * just before the call can never make us sleep through the shutdown. */
    struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };

    while (!g_stop) {
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
        spawn_client(cfd, &ca);
    }

    /* ---- graceful shutdown ---- */
    log_line("INFO", NULL, "shutting down...");
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

    if (left > 0)
        log_line("WARN", NULL, "%d worker(s) still running at exit", left);
    log_line("INFO", NULL, "server stopped");
    return 0;
}