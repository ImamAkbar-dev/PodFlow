/**
 * @file stream_session.c
 * @brief Per-connection stream session: state machine + producer thread.
 *
 * Contract: PodFlow v1.1 (include/stream_session.h). Owner: Member 2.
 *
 * THREADS
 *   connection thread : calls session_play/pause/seek/stop/next/state.
 *   producer thread   : one per session, started lazily by the first successful
 *                       PLAY, joined in session_destroy(). It reads the WAV file
 *                       and pushes items into the ring buffer (40 slots = 2 s).
 *
 * LOCKING
 *   `s->mtx` guards EVERY field of the session, including the file descriptor.
 *   Lock order: s->mtx (outer) -> ring buffer mutex (inner). The producer never
 *   holds s->mtx while it blocks in rb_push(), so a full ring can never block
 *   a command (PAUSE/SEEK/STOP) of the connection thread.
 *   The producer reads a chunk with pread() while holding s->mtx. That costs a
 *   command at most one 8.8 KB read, and it makes the lifetime of the file
 *   descriptor trivial: only code holding s->mtx can use or close it.
 *
 * EPOCH PROTOCOL (no stale data after SEEK/STOP/PLAY)
 *   In ONE critical section the producer (a) reads the cursor, (b) reads the
 *   chunk, (c) advances the cursor and (d) captures rb_epoch(). It then pushes
 *   OUTSIDE the lock with that epoch. SEEK/STOP/PLAY change the cursor and call
 *   rb_flush() (epoch++) inside their own critical section, so an item produced
 *   before the command is either flushed from the ring or rejected by rb_push()
 *   (it returns 1 = stale epoch). Nothing from the old position can survive.
 *
 * RING BUFFER ITEMS
 *   tag 0            -> 44-byte WAV header (ITEM_HEADER)
 *   tag 1..N         -> PCM chunk number tag (ITEM_CHUNK)
 *   tag RB_TAG_END   -> end of stream (ITEM_END), len 0
 *
 * BOOKKEEPING happens when the CONNECTION thread pops an item (session_next),
 * not when the producer reads ahead:
 *   next_send_seq = (last popped chunk) + 1, or the seek target
 *   last_sent_seq = last popped chunk (0 if none yet)
 *   bytes_popped  = header + chunk bytes popped since PLAY -> END.param
 * so PAUSE/STOP report what the client really received, not what is buffered.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "stream_session.h"

#include "ring_buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_MEDIA_DIR     "./media"
#define MAX_NAME_LEN          255U
#define MAX_TOTAL_CHUNKS      0xFFFFFFF0ULL
#define PRODUCER_STACK_BYTES  (128 * 1024)

#define TAG_HEADER            0U            /* chunk numbers start at 1 */

struct stream_session {
    char           *media_dir;
    pthread_mutex_t mtx;                    /* guards everything below  */
    pthread_cond_t  cv;                     /* wakes an idle producer   */
    ring_buffer_t  *rb;

    pthread_t       producer;
    bool            producer_started;
    bool            shutdown;               /* set by session_destroy() */

    sess_state_t    state;

    /* current stream (valid while state != SESS_IDLE) */
    int             fd;                     /* -1 when there is no stream */
    uint8_t         wav_header[WAV_HEADER_SIZE];
    uint64_t        data_bytes;             /* file size - 44             */
    uint32_t        total_chunks;

    /* consumer-side bookkeeping (updated in session_next) */
    uint32_t        next_send_seq;
    uint32_t        last_sent_seq;
    uint64_t        bytes_popped;
    bool            header_popped;

    /* producer cursor */
    bool            active;                 /* producer has work to do          */
    bool            prod_header_pending;    /* WAV header still to be produced  */
    bool            prod_eof;               /* END marker already produced      */
    uint32_t        prod_next_seq;          /* next chunk the producer reads    */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Canonical PCM / 16-bit / 44100 Hz / stereo WAV header (44 bytes). */
static bool wav_header_ok(const uint8_t *h)
{
    return memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0 &&
           memcmp(h + 12, "fmt ", 4) == 0 && le32(h + 16) == 16 &&
           le16(h + 20) == 1 && le16(h + 22) == 2 && le32(h + 24) == 44100 &&
           le16(h + 34) == 16 && memcmp(h + 36, "data", 4) == 0;
}

/* A bare file name: no path separators, no "..", no NUL, bounded length. */
static bool name_ok(const char *name, uint32_t len)
{
    if (name == NULL || len == 0 || len > MAX_NAME_LEN)
        return false;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '\0' || name[i] == '/')
            return false;
        if (name[i] == '.' && i + 1 < len && name[i + 1] == '.')
            return false;
    }
    return true;
}

/* Read up to n bytes at offset; retries EINTR and short reads. Returns bytes read, or -1. */
static ssize_t pread_full(int fd, void *buf, size_t n, off_t off)
{
    size_t got = 0;

    while (got < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, off + (off_t)got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return got > 0 ? (ssize_t)got : -1;
        }
        if (r == 0)
            break;                          /* EOF: file shorter than expected */
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Back to IDLE: producer goes quiet, file closed. Caller holds s->mtx. */
static void end_stream_locked(stream_session_t *s)
{
    s->state = SESS_IDLE;
    s->active = false;
    s->prod_eof = false;
    s->prod_header_pending = false;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Producer thread                                                     */
/* ------------------------------------------------------------------ */

static void *producer_main(void *arg)
{
    stream_session_t *s = arg;
    uint8_t buf[PCM_CHUNK_SIZE];

    pthread_mutex_lock(&s->mtx);
    while (!s->shutdown) {
        uint32_t tag, len, epoch;

        if (!s->active || s->prod_eof) {
            pthread_cond_wait(&s->cv, &s->mtx);
            continue;
        }

        /* ---- one critical section: cursor, data, epoch ---- */
        epoch = rb_epoch(s->rb);
        len = 0;
        tag = RB_TAG_END;

        if (s->prod_header_pending) {
            memcpy(buf, s->wav_header, WAV_HEADER_SIZE);
            len = WAV_HEADER_SIZE;
            tag = TAG_HEADER;
            s->prod_header_pending = false;
        } else if (s->prod_next_seq <= s->total_chunks) {
            uint64_t off = (uint64_t)(s->prod_next_seq - 1) * PCM_CHUNK_SIZE;
            uint64_t remaining = s->data_bytes - off;
            size_t want = remaining < PCM_CHUNK_SIZE ? (size_t)remaining : PCM_CHUNK_SIZE;
            ssize_t got = pread_full(s->fd, buf, want, (off_t)(WAV_HEADER_SIZE + off));

            if (got > 0) {
                len = (uint32_t)got;
                tag = s->prod_next_seq++;
            } else {
                /* file vanished or got truncated: end the stream cleanly */
                s->prod_next_seq = s->total_chunks + 1;
                s->prod_eof = true;
            }
        } else {
            s->prod_eof = true;             /* all chunks produced: emit END */
        }
        /* ---------------------------------------------------- */

        pthread_mutex_unlock(&s->mtx);
        int rc = rb_push(s->rb, buf, len, tag, epoch);   /* may block: no lock held */
        pthread_mutex_lock(&s->mtx);

        if (rc < 0)                          /* ring closed: session is going away */
            break;
        /* rc == 1: stale epoch, a command changed the cursor; just loop again. */
    }
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

stream_session_t *session_create(const char *media_dir)
{
    stream_session_t *s = calloc(1, sizeof *s);
    int mtx_ok = 0;

    if (s == NULL)
        return NULL;
    s->fd = -1;
    s->state = SESS_IDLE;
    s->media_dir = strdup(media_dir != NULL ? media_dir : DEFAULT_MEDIA_DIR);
    s->rb = rb_create(RB_DEFAULT_SLOTS, PCM_CHUNK_SIZE);
    if (s->media_dir == NULL || s->rb == NULL)
        goto fail;
    if (pthread_mutex_init(&s->mtx, NULL) != 0)
        goto fail;
    mtx_ok = 1;
    if (pthread_cond_init(&s->cv, NULL) != 0)
        goto fail;
    return s;

fail:
    if (mtx_ok)
        pthread_mutex_destroy(&s->mtx);
    rb_destroy(s->rb);
    free(s->media_dir);
    free(s);
    return NULL;
}

void session_destroy(stream_session_t *s)
{
    if (s == NULL)
        return;

    pthread_mutex_lock(&s->mtx);
    s->shutdown = true;
    s->active = false;
    rb_close(s->rb);                         /* wakes a producer blocked in rb_push() */
    pthread_cond_broadcast(&s->cv);          /* wakes an idle producer                */
    pthread_mutex_unlock(&s->mtx);

    if (s->producer_started)
        pthread_join(s->producer, NULL);

    if (s->fd >= 0)
        close(s->fd);
    rb_destroy(s->rb);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
    free(s->media_dir);
    free(s);
}

sess_state_t session_state(stream_session_t *s)
{
    pthread_mutex_lock(&s->mtx);
    sess_state_t st = s->state;
    pthread_mutex_unlock(&s->mtx);
    return st;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

/* PLAY with a file name, from IDLE. Caller holds s->mtx. */
static proto_err_t start_stream_locked(stream_session_t *s, const char *name,
                                       uint32_t name_len, uint32_t start_chunk,
                                       uint32_t *ack_param)
{
    char path[PATH_MAX];
    uint8_t hdr[WAV_HEADER_SIZE];
    struct stat st;
    int fd, n;

    if (!name_ok(name, name_len))
        return ERR_FILE_NOT_FOUND;
    n = snprintf(path, sizeof path, "%s/%.*s", s->media_dir, (int)name_len, name);
    if (n < 0 || (size_t)n >= sizeof path)
        return ERR_FILE_NOT_FOUND;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return ERR_FILE_NOT_FOUND;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return ERR_FILE_NOT_FOUND;
    }
    if (st.st_size <= (off_t)WAV_HEADER_SIZE ||
        pread_full(fd, hdr, WAV_HEADER_SIZE, 0) != (ssize_t)WAV_HEADER_SIZE ||
        !wav_header_ok(hdr)) {
        close(fd);
        return ERR_INVALID_WAV;
    }

    uint64_t data_bytes = (uint64_t)st.st_size - WAV_HEADER_SIZE;
    uint64_t total = (data_bytes + PCM_CHUNK_SIZE - 1) / PCM_CHUNK_SIZE;

    if (total > MAX_TOTAL_CHUNKS) {
        close(fd);
        return ERR_INVALID_WAV;
    }
    if (start_chunk > total) {               /* 0 and 1 both mean "from the start" */
        close(fd);
        return ERR_SEEK_OUT_OF_RANGE;
    }
    uint32_t effective = start_chunk == 0 ? 1U : start_chunk;

    /* Spawn the producer lazily (it will wait on s->cv until s->active is set). */
    if (!s->producer_started) {
        pthread_attr_t attr;
        int err;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, PRODUCER_STACK_BYTES);
        err = pthread_create(&s->producer, &attr, producer_main, s);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            close(fd);
            return ERR_INTERNAL_SERVER;
        }
        s->producer_started = true;
    }

    s->fd = fd;
    memcpy(s->wav_header, hdr, WAV_HEADER_SIZE);
    s->data_bytes = data_bytes;
    s->total_chunks = (uint32_t)total;
    s->next_send_seq = effective;
    s->last_sent_seq = 0;
    s->bytes_popped = 0;
    s->header_popped = false;
    s->prod_next_seq = effective;
    s->prod_header_pending = true;
    s->prod_eof = false;
    s->active = true;
    rb_flush(s->rb);                         /* drop leftovers + reject in-flight old items */
    s->state = SESS_PLAYING;
    pthread_cond_signal(&s->cv);
    *ack_param = effective;
    return ERR_NONE;
}

proto_err_t session_play(stream_session_t *s, const char *name, uint32_t name_len,
                         uint32_t start_chunk, uint32_t *ack_param)
{
    uint32_t dummy = 0;
    proto_err_t rc;

    if (ack_param == NULL)
        ack_param = &dummy;

    pthread_mutex_lock(&s->mtx);
    if (name_len == 0) {                     /* resume */
        if (s->state != SESS_PAUSED) {
            rc = ERR_INVALID_STATE;
        } else {
            s->state = SESS_PLAYING;
            *ack_param = s->next_send_seq;
            rc = ERR_NONE;
        }
    } else if (s->state != SESS_IDLE) {
        rc = ERR_INVALID_STATE;
    } else {
        rc = start_stream_locked(s, name, name_len, start_chunk, ack_param);
    }
    pthread_mutex_unlock(&s->mtx);
    return rc;
}

proto_err_t session_pause(stream_session_t *s, uint32_t *ack_param)
{
    uint32_t dummy = 0;
    proto_err_t rc = ERR_NONE;

    if (ack_param == NULL)
        ack_param = &dummy;

    pthread_mutex_lock(&s->mtx);
    if (s->state != SESS_PLAYING) {
        rc = ERR_INVALID_STATE;
    } else {
        s->state = SESS_PAUSED;              /* the producer keeps filling the ring */
        *ack_param = s->next_send_seq;
    }
    pthread_mutex_unlock(&s->mtx);
    return rc;
}

proto_err_t session_seek(stream_session_t *s, uint32_t target, uint32_t *ack_param)
{
    uint32_t dummy = 0;
    proto_err_t rc = ERR_NONE;

    if (ack_param == NULL)
        ack_param = &dummy;

    pthread_mutex_lock(&s->mtx);
    if (s->state == SESS_IDLE) {
        rc = ERR_INVALID_STATE;
    } else if (target < 1 || target > s->total_chunks) {
        rc = ERR_SEEK_OUT_OF_RANGE;          /* nothing was modified */
    } else {
        s->prod_next_seq = target;
        s->prod_eof = false;                 /* a queued END is about to be flushed */
        /* The header is never resent, unless the client has not received it yet. */
        s->prod_header_pending = !s->header_popped;
        rb_flush(s->rb);
        s->next_send_seq = target;
        pthread_cond_signal(&s->cv);
        *ack_param = target;                 /* state (PLAYING / PAUSED) is unchanged */
    }
    pthread_mutex_unlock(&s->mtx);
    return rc;
}

proto_err_t session_stop(stream_session_t *s, uint32_t *ack_param)
{
    uint32_t dummy = 0;
    proto_err_t rc = ERR_NONE;

    if (ack_param == NULL)
        ack_param = &dummy;

    pthread_mutex_lock(&s->mtx);
    if (s->state == SESS_IDLE) {
        rc = ERR_INVALID_STATE;
    } else {
        *ack_param = s->last_sent_seq;
        end_stream_locked(s);
        rb_flush(s->rb);                     /* also rejects an in-flight producer item */
    }
    pthread_mutex_unlock(&s->mtx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Consumer side                                                       */
/* ------------------------------------------------------------------ */

int session_next(stream_session_t *s, session_item_t *out)
{
    uint32_t len = 0, tag = 0;
    int got = 0;

    out->kind = ITEM_NONE;
    out->seq = 0;
    out->param = 0;
    out->len = 0;

    pthread_mutex_lock(&s->mtx);
    if (s->state == SESS_PLAYING && rb_pop(s->rb, out->data, &len, &tag) == 1) {
        got = 1;
        if (tag == RB_TAG_END) {
            out->kind = ITEM_END;
            out->seq = s->total_chunks + 1U;
            out->param = (uint32_t)s->bytes_popped;
            out->len = 0;
            end_stream_locked(s);            /* state returns to IDLE */
        } else if (tag == TAG_HEADER) {
            out->kind = ITEM_HEADER;
            out->seq = 0;
            out->len = len;
            s->header_popped = true;
            s->bytes_popped += len;
        } else {
            out->kind = ITEM_CHUNK;
            out->seq = tag;
            out->len = len;
            s->last_sent_seq = tag;
            s->next_send_seq = tag + 1U;
            s->bytes_popped += len;
        }
    }
    pthread_mutex_unlock(&s->mtx);
    return got;
}