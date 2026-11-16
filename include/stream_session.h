/**
 * @file stream_session.h
 * @brief Per-Connection Stream Session State Machine & Producer Interface.
 *
 * Owner: Member 2 (Protocol, Ring Buffer, Stream Session)
 * Standard: C11, POSIX Threads
 *
 * LOCK ORDERING INVARIANT:
 *   `session->mtx` (outer) ---> `ring_buffer->mtx` (inner).
 *   A thread holding `ring_buffer->mtx` MUST NEVER acquire `session->mtx`.
 */

#ifndef PODFLOW_STREAM_SESSION_H
#define PODFLOW_STREAM_SESSION_H

#include "protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-connection playback state machine states.
 */
typedef enum { SESS_IDLE = 0, SESS_PLAYING = 1, SESS_PAUSED = 2 } sess_state_t;

/**
 * @brief Discriminator for items returned by session_next().
 */
typedef enum {
  ITEM_NONE = 0,
  ITEM_HEADER = 1,
  ITEM_CHUNK = 2,
  ITEM_END = 3
} item_kind_t;

/**
 * @brief Single emission unit returned by session_next() to the connection
 * thread.
 */
typedef struct {
  item_kind_t kind;
  uint32_t seq;
  uint32_t param;
  uint32_t len;
  uint8_t data[PCM_CHUNK_SIZE];
} session_item_t;

/**
 * @brief Opaque stream session handle (one per client connection).
 */
typedef struct stream_session stream_session_t;

/**
 * @brief Create a new stream session bound to a media directory (e.g.,
 * "./media").
 *
 * Allocates the internal 40-slot ring buffer (`RB_DEFAULT_SLOTS`, 2 seconds).
 * The producer thread is spawned lazily on the first successful PLAY command.
 *
 * @param media_dir Path to directory containing .wav files (defaults to
 * "./media" if NULL).
 * @return Pointer to stream_session_t, or NULL on allocation error.
 */
stream_session_t *session_create(const char *media_dir);

/**
 * @brief Stop any active stream, join the producer thread if spawned, destroy
 *        the ring buffer, and free all session resources.
 *
 * @param s Stream session handle (safe if NULL).
 */
void session_destroy(stream_session_t *s);

/**
 * @brief Thread-safe query of the current session state.
 */
sess_state_t session_state(stream_session_t *s);

/**
 * @brief Handle a PLAY command (either initial play from IDLE or resume from
 * PAUSED).
 *
 * - If `name_len == 0` (resume):
 *   Must be in `SESS_PAUSED`. Transitions to `SESS_PLAYING` and sets
 *   `*ack_param = next_send_seq`. Otherwise returns `ERR_INVALID_STATE` (300).
 * - If `name_len > 0` (start new stream):
 *   Must be in `SESS_IDLE` (otherwise returns `ERR_INVALID_STATE` = 300).
 *   Validates filename (no '/', "..", or NUL bytes; must exist in media_dir ->
 * 200), validates canonical 44-byte WAV header ("RIFF", "WAVE", "data" at 36,
 * PCM=1, 2 ch, 44100 Hz, 16-bit, filesize > 44 -> 201), validates `start_chunk`
 *   (0 or 1..total_chunks -> 202), lazily spawns or wakes producer at
 *   `effective_start = (start_chunk == 0) ? 1 : start_chunk`, and sets
 *   `*ack_param = effective_start`.
 *
 * @param s           Stream session handle.
 * @param name        UTF-8 filename (need not be NUL-terminated; may be NULL
 * iff name_len == 0).
 * @param name_len    Length of filename in bytes (0 indicates RESUME).
 * @param start_chunk Requested starting chunk (0 or 1 = from
 * start; 1..total_chunks).
 * @param ack_param   Out-parameter receiving the ACK param value on ERR_NONE.
 * @return ERR_NONE (0) on success, or proto_err_t (200, 201, 202, 300, 500).
 */
proto_err_t session_play(stream_session_t *s, const char *name,
                         uint32_t name_len, uint32_t start_chunk,
                         uint32_t *ack_param);

/**
 * @brief Handle a PAUSE command.
 *
 * Valid ONLY in `SESS_PLAYING` (otherwise returns `ERR_INVALID_STATE` = 300).
 * Transitions to `SESS_PAUSED` and sets `*ack_param = next_send_seq`.
 * Producer continues filling the ring buffer until full (back-pressure) so
 * subsequent resume is instantaneous.
 */
proto_err_t session_pause(stream_session_t *s, uint32_t *ack_param);

/**
 * @brief Handle a SEEK command.
 *
 * Valid in `SESS_PLAYING` or `SESS_PAUSED` (returns `ERR_INVALID_STATE` = 300
 * in IDLE). Validates `1 <= target <= total_chunks` (returns
 * `ERR_SEEK_OUT_OF_RANGE` = 202 leaving state and stream unchanged if out of
 * range). Computes `offset = 44 + (target - 1) * 8820`, calls `fseeko`, flushes
 * the ring buffer (`rb_flush`), sets `next_send_seq = target`, wakes the
 * producer, and sets `*ack_param = target`. Does NOT resend the 44-byte WAV
 * header. Preserves `SESS_PAUSED` if called while paused.
 */
proto_err_t session_seek(stream_session_t *s, uint32_t target,
                         uint32_t *ack_param);

/**
 * @brief Handle a STOP command.
 *
 * Valid in `SESS_PLAYING` or `SESS_PAUSED` (returns `ERR_INVALID_STATE` = 300
 * in IDLE). Flushes the ring buffer, stops the producer from reading further,
 * closes the WAV file, transitions to `SESS_IDLE`, and sets `*ack_param =
 * last_sent_seq`. No END frame is emitted after STOP.
 */
proto_err_t session_stop(stream_session_t *s, uint32_t *ack_param);

/**
 * @brief Non-blocking retrieval of the next item to transmit on a pacing tick.
 *
 * If state is not `SESS_PLAYING`, immediately returns `0` (`out->kind =
 * ITEM_NONE`). Order of emission after PLAY:
 *   1. `ITEM_HEADER` (`seq = 0`, `param = 0`, `len = 44`, 44-byte WAV header)
 *   2. `ITEM_CHUNK`  (`seq = n`, `param = 0`, `len <= 8820`, raw PCM chunk)
 *   3. `ITEM_END`    (`seq = total_chunks + 1`, `param = total payload bytes
 * sent since PLAY including the 44-byte header`, `len = 0`), and automatically
 * transitions session state to `SESS_IDLE`.
 *
 * If the ring buffer is temporarily empty (producer underrun), returns `0` with
 * `out->kind = ITEM_NONE` while remaining in `SESS_PLAYING`.
 *
 * @param s   Stream session handle.
 * @param out Out-parameter receiving the item (non-NULL).
 * @return 1 if an item was populated in `*out`; 0 if no item is available now.
 */
int session_next(stream_session_t *s, session_item_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PODFLOW_STREAM_SESSION_H */