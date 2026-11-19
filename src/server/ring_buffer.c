/**
 * @file ring_buffer.c
 * @brief Implementation of the bounded, epoch-aware ring buffer.
 *
 * Owner: Member 2 (Protocol, Ring Buffer, Stream Session)
 */

#include "ring_buffer.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t len;
    uint32_t tag;
    uint8_t *data;
} rb_slot_t;

struct ring_buffer {
    rb_slot_t      *slots;
    uint8_t        *data_arena;
    size_t          capacity;
    size_t          slot_size;
    size_t          head;       /* Next index to pop (consumer)  */
    size_t          tail;       /* Next index to push (producer) */
    size_t          count;      /* Number of occupied slots      */
    uint32_t        epoch;      /* Generation counter for flush  */
    bool            closed;     /* True after rb_close()         */
    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
};

ring_buffer_t *rb_create(size_t slots, size_t slot_size)
{
    if (slots == 0 || slot_size == 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Check for multiplication overflow in slots * slot_size */
    if (slots > SIZE_MAX / slot_size) {
        errno = ENOMEM;
        return NULL;
    }

    ring_buffer_t *rb = (ring_buffer_t *)calloc(1, sizeof(ring_buffer_t));
    if (rb == NULL) {
        return NULL;
    }

    rb->slots = (rb_slot_t *)calloc(slots, sizeof(rb_slot_t));
    if (rb->slots == NULL) {
        free(rb);
        return NULL;
    }

    /* Allocate a single contiguous memory arena for all slot payload buffers
     * to improve cache locality and avoid N separate malloc/free calls. */
    rb->data_arena = (uint8_t *)calloc(slots, slot_size);
    if (rb->data_arena == NULL) {
        free(rb->slots);
        free(rb);
        return NULL;
    }

    for (size_t i = 0; i < slots; i++) {
        rb->slots[i].data = rb->data_arena + (i * slot_size);
    }

    rb->capacity  = slots;
    rb->slot_size = slot_size;
    rb->head      = 0;
    rb->tail      = 0;
    rb->count     = 0;
    rb->epoch     = 1; /* Start at epoch 1 */
    rb->closed    = false;

    if (pthread_mutex_init(&rb->mtx, NULL) != 0) {
        free(rb->data_arena);
        free(rb->slots);
        free(rb);
        return NULL;
    }

    if (pthread_cond_init(&rb->not_full, NULL) != 0) {
        pthread_mutex_destroy(&rb->mtx);
        free(rb->data_arena);
        free(rb->slots);
        free(rb);
        return NULL;
    }

    if (pthread_cond_init(&rb->not_empty, NULL) != 0) {
        pthread_cond_destroy(&rb->not_full);
        pthread_mutex_destroy(&rb->mtx);
        free(rb->data_arena);
        free(rb->slots);
        free(rb);
        return NULL;
    }

    return rb;
}

void rb_destroy(ring_buffer_t *rb)
{
    if (rb == NULL) {
        return;
    }

    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_mutex_destroy(&rb->mtx);

    free(rb->data_arena);
    free(rb->slots);
    free(rb);
}

int rb_push(ring_buffer_t *rb, const void *data, uint32_t len,
            uint32_t tag, uint32_t epoch)
{
    if (rb == NULL || len > rb->slot_size || (len > 0 && data == NULL)) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&rb->mtx);

    /* Wait while the ring buffer is full, BUT wake immediately if:
     *   1. rb_close() is called (rb->closed == true), OR
     *   2. rb_flush() incremented rb->epoch (epoch != rb->epoch). */
    while (rb->count == rb->capacity && !rb->closed && epoch == rb->epoch) {
        pthread_cond_wait(&rb->not_full, &rb->mtx);
    }

    if (rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    if (epoch != rb->epoch) {
        /* Stale generation epoch: discard item without modifying ring state */
        pthread_mutex_unlock(&rb->mtx);
        return 1;
    }

    rb_slot_t *slot = &rb->slots[rb->tail];
    slot->len = len;
    slot->tag = tag;
    if (len > 0) {
        memcpy(slot->data, data, (size_t)len);
    }

    rb->tail = (rb->tail + 1U) % rb->capacity;
    rb->count++;

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int rb_pop(ring_buffer_t *rb, void *out, uint32_t *len, uint32_t *tag)
{
    if (rb == NULL || len == NULL || tag == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&rb->mtx);

    if (rb->count == 0) {
        bool is_closed = rb->closed;
        pthread_mutex_unlock(&rb->mtx);
        return is_closed ? -1 : 0;
    }

    rb_slot_t *slot = &rb->slots[rb->head];
    uint32_t item_len = slot->len;
    if (item_len > 0 && out == NULL) {
        pthread_mutex_unlock(&rb->mtx);
        errno = EINVAL;
        return -1;
    }

    *len = item_len;
    *tag = slot->tag;
    if (item_len > 0) {
        memcpy(out, slot->data, (size_t)item_len);
    }

    rb->head = (rb->head + 1U) % rb->capacity;
    rb->count--;

    /* Wake a blocked producer waiting for space */
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return 1;
}

uint32_t rb_flush(ring_buffer_t *rb)
{
    if (rb == NULL) {
        return 0;
    }

    pthread_mutex_lock(&rb->mtx);
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    rb->epoch++;
    uint32_t new_epoch = rb->epoch;

    /* Broadcast to wake all producers blocked in rb_push() so they observe
     * the new epoch or freed space immediately. */
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);

    return new_epoch;
}

void rb_close(ring_buffer_t *rb)
{
    if (rb == NULL) {
        return;
    }

    pthread_mutex_lock(&rb->mtx);
    rb->closed = true;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
}

size_t rb_count(ring_buffer_t *rb)
{
    if (rb == NULL) {
        return 0;
    }

    pthread_mutex_lock(&rb->mtx);
    size_t c = rb->count;
    pthread_mutex_unlock(&rb->mtx);
    return c;
}

uint32_t rb_epoch(ring_buffer_t *rb)
{
    if (rb == NULL) {
        return 0;
    }

    pthread_mutex_lock(&rb->mtx);
    uint32_t e = rb->epoch;
    pthread_mutex_unlock(&rb->mtx);
    return e;
}