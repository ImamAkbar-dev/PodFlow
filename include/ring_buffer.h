/**
 * @file ring_buffer.h
 * @brief Bounded, Epoch-Aware, Thread-Safe Ring Buffer for PodFlow (Session 2+3).
 *
 * Owner: Member 2 (Protocol, Ring Buffer, Stream Session)
 * Standard: C11, POSIX Threads (pthread_mutex_t, pthread_cond_t)
 */

#ifndef PODFLOW_RING_BUFFER_H
#define PODFLOW_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Special tag value used by the producer to mark End-Of-Stream (EOF)
 *        inside a ring buffer slot.
 */
#define RB_TAG_END 0xFFFFFFFFU

/**
 * @brief Default number of slots per stream session (40 slots * 50 ms = 2.0 s).
 */
#define RB_DEFAULT_SLOTS 40U

/**
 * @brief Opaque ring buffer handle.
 */
typedef struct ring_buffer ring_buffer_t;

/**
 * @brief Allocate and initialize a bounded ring buffer.
 *
 * @param slots     Number of slots in the circular queue (must be >= 1).
 * @param slot_size Maximum byte capacity of each slot (e.g., PCM_CHUNK_SIZE = 8820).
 * @return Pointer to allocated ring_buffer_t, or NULL on invalid args / OOM.
 */
ring_buffer_t *rb_create(size_t slots, size_t slot_size);

/**
 * @brief Destroy a ring buffer and free all associated memory and sync primitives.
 *        Caller must ensure no threads are concurrently blocked in rb_push().
 *
 * @param rb Ring buffer handle (safe if NULL).
 */
void rb_destroy(ring_buffer_t *rb);

/**
 * @brief Push a chunk (or END marker) into the ring buffer.
 *
 * Blocks on the internal `not_full` condition variable while the ring buffer is
 * full, PROVIDED the buffer remains open and `epoch == rb_epoch(rb)`.
 *
 * - If `epoch != rb->epoch` upon entry OR changes while blocked (due to a
 *   concurrent `rb_flush()`), the item is immediately discarded without modifying
 *   the buffer and the function returns `1`.
 * - If the buffer is closed via `rb_close()`, returns `-1`.
 *
 * @param rb    Ring buffer handle.
 * @param data  Pointer to payload bytes (may be NULL iff len == 0).
 * @param len   Byte length of data (must be <= slot_size).
 * @param tag   Sequence number of the chunk, or RB_TAG_END for EOF marker.
 * @param epoch Producer's generation epoch captured before reading the chunk.
 * @return  0 on successful enqueue;
 *          1 if discarded due to stale epoch (`epoch != rb->epoch`);
 *         -1 on closed buffer or invalid arguments.
 */
int rb_push(ring_buffer_t *rb, const void *data, uint32_t len,
            uint32_t tag, uint32_t epoch);

/**
 * @brief Non-blocking pop of the oldest item from the ring buffer.
 *
 * Never blocks. Called by the connection thread when a pacing tick is due.
 * Signals `not_full` when a slot is freed so a blocked producer can resume.
 *
 * @param rb  Ring buffer handle.
 * @param out Destination buffer of at least `slot_size` bytes (may be NULL if caller only wants len/tag of 0-len item).
 * @param len Out-parameter receiving payload length in bytes (non-NULL).
 * @param tag Out-parameter receiving item tag / sequence number (non-NULL).
 * @return  1 if an item was popped;
 *          0 if the buffer is currently empty (and not closed);
 *         -1 if the buffer is closed AND empty (or on invalid arguments).
 */
int rb_pop(ring_buffer_t *rb, void *out, uint32_t *len, uint32_t *tag);

/**
 * @brief Atomically discard all queued items, increment the epoch counter,
 *        and wake all blocked producer threads.
 *
 * Used on SEEK and STOP to immediately invalidate queued chunks and any
 * in-flight chunk currently being read by the producer thread.
 *
 * @param rb Ring buffer handle.
 * @return The new epoch value after incrementing (or 0 if rb is NULL).
 */
uint32_t rb_flush(ring_buffer_t *rb);

/**
 * @brief Permanently close the ring buffer and wake all waiting threads.
 *
 * Subsequent `rb_push()` calls return `-1`. Subsequent `rb_pop()` calls drain
 * any remaining items and then return `-1` once empty.
 *
 * @param rb Ring buffer handle.
 */
void rb_close(ring_buffer_t *rb);

/**
 * @brief Return the current number of items stored in the ring buffer.
 */
size_t rb_count(ring_buffer_t *rb);

/**
 * @brief Return the current epoch counter of the ring buffer.
 */
uint32_t rb_epoch(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* PODFLOW_RING_BUFFER_H */