/**
 * @file test_ring_buffer.c
 * @brief Unit, concurrency, epoch-flush, and 1,000,000-item stress tests for
 * ring_buffer.c.
 *
 * Owner: Member 2 (Protocol, Ring Buffer, Stream Session)
 */

#include "ring_buffer.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    printf("  [RUN ] %-48s ... ", #fn);                                        \
    fflush(stdout);                                                            \
    fn();                                                                      \
    printf("PASS\n");                                                          \
  } while (0)

/* ========================================================================== *
 * TEST 1: Single-thread wrap-around, full/empty, and count invariants        *
 * ========================================================================== */
static void test_single_thread_wrap_around(void) {
  const size_t slots = 4;
  const size_t slot_sz = 64;
  ring_buffer_t *rb = rb_create(slots, slot_sz);
  assert(rb != NULL);
  assert(rb_count(rb) == 0);

  uint32_t epoch = rb_epoch(rb);
  uint8_t tx[64];
  uint8_t rx[64];
  uint32_t len = 0, tag = 0;

  /* Pop on empty must return 0 without blocking */
  assert(rb_pop(rb, rx, &len, &tag) == 0);

  /* Fill 4 slots, pop 2, push 2 more to force head/tail wrap-around 5 times */
  uint32_t next_push = 1;
  uint32_t next_pop = 1;

  for (int cycle = 0; cycle < 5; cycle++) {
    while (rb_count(rb) < slots) {
      memset(tx, (int)(next_push & 0xFFU), sizeof(tx));
      assert(rb_push(rb, tx, 32, next_push, epoch) == 0);
      next_push++;
    }
    assert(rb_count(rb) == slots);

    /* Pop 2 items */
    for (int p = 0; p < 2; p++) {
      memset(rx, 0, sizeof(rx));
      assert(rb_pop(rb, rx, &len, &tag) == 1);
      assert(len == 32);
      assert(tag == next_pop);
      for (size_t b = 0; b < 32; b++) {
        assert(rx[b] == (uint8_t)(next_pop & 0xFFU));
      }
      next_pop++;
    }
    assert(rb_count(rb) == slots - 2);
  }

  /* Drain remaining items */
  while (rb_count(rb) > 0) {
    assert(rb_pop(rb, rx, &len, &tag) == 1);
    assert(tag == next_pop);
    next_pop++;
  }
  assert(rb_pop(rb, rx, &len, &tag) == 0);

  rb_destroy(rb);
}

/* ========================================================================== *
 * TEST 2: Blocking push when full, released by consumer pop                  *
 * ========================================================================== */
typedef struct {
  ring_buffer_t *rb;
  uint32_t epoch;
  int push_rc;
  bool completed;
  pthread_mutex_t mtx;
} block_push_arg_t;

static void *blocking_pusher_thread(void *arg) {
  block_push_arg_t *ba = (block_push_arg_t *)arg;
  uint32_t payload = 0xCAFEBABEU;
  int rc = rb_push(ba->rb, &payload, sizeof(payload), 99, ba->epoch);

  pthread_mutex_lock(&ba->mtx);
  ba->push_rc = rc;
  ba->completed = true;
  pthread_mutex_unlock(&ba->mtx);
  return NULL;
}

static void test_blocking_push_released_by_pop(void) {
  ring_buffer_t *rb = rb_create(2, 16);
  assert(rb != NULL);
  uint32_t epoch = rb_epoch(rb);

  uint32_t v1 = 111, v2 = 222;
  assert(rb_push(rb, &v1, sizeof(v1), 1, epoch) == 0);
  assert(rb_push(rb, &v2, sizeof(v2), 2, epoch) == 0);
  assert(rb_count(rb) == 2);

  block_push_arg_t ba = {.rb = rb,
                         .epoch = epoch,
                         .push_rc = -99,
                         .completed = false,
                         .mtx = PTHREAD_MUTEX_INITIALIZER};

  pthread_t tid;
  assert(pthread_create(&tid, NULL, blocking_pusher_thread, &ba) == 0);

  /* Sleep 20 ms to let pusher thread block on full buffer */
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&ba.mtx);
  assert(ba.completed == false);
  pthread_mutex_unlock(&ba.mtx);

  /* Now pop 1 item -> must wake the blocked pusher */
  uint32_t out = 0, len = 0, tag = 0;
  assert(rb_pop(rb, &out, &len, &tag) == 1);
  assert(tag == 1 && out == 111);

  pthread_join(tid, NULL);
  assert(ba.completed == true);
  assert(ba.push_rc == 0);
  assert(rb_count(rb) == 2);

  pthread_mutex_destroy(&ba.mtx);
  rb_destroy(rb);
}

/* ========================================================================== *
 * TEST 3: Flush with stale-epoch discard (both queued and blocked producer)  *
 * ========================================================================== */
static void test_flush_and_stale_epoch_discard(void) {
  ring_buffer_t *rb = rb_create(2, 16);
  assert(rb != NULL);
  uint32_t old_epoch = rb_epoch(rb);

  uint32_t v = 42;
  assert(rb_push(rb, &v, sizeof(v), 1, old_epoch) == 0);
  assert(rb_push(rb, &v, sizeof(v), 2, old_epoch) == 0);

  /* Spawn a pusher blocked on full buffer with old_epoch */
  block_push_arg_t ba = {.rb = rb,
                         .epoch = old_epoch,
                         .push_rc = -99,
                         .completed = false,
                         .mtx = PTHREAD_MUTEX_INITIALIZER};
  pthread_t tid;
  assert(pthread_create(&tid, NULL, blocking_pusher_thread, &ba) == 0);

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
  nanosleep(&ts, NULL);

  /* Flush the buffer -> increments epoch and wakes blocked pusher */
  uint32_t new_epoch = rb_flush(rb);
  assert(new_epoch == old_epoch + 1U);

  pthread_join(tid, NULL);
  /* Blocked pusher must return 1 (discarded due to stale epoch), leaving count
   * == 0 */
  assert(ba.push_rc == 1);
  assert(rb_count(rb) == 0);

  /* A direct push with old_epoch must also be discarded immediately (return 1)
   */
  assert(rb_push(rb, &v, sizeof(v), 3, old_epoch) == 1);
  assert(rb_count(rb) == 0);

  /* A push with new_epoch succeeds (return 0) */
  assert(rb_push(rb, &v, sizeof(v), 10, new_epoch) == 0);
  assert(rb_count(rb) == 1);

  pthread_mutex_destroy(&ba.mtx);
  rb_destroy(rb);
}

/* ========================================================================== *
 * TEST 4: Close semantics (wakes blocked pusher, drains remaining items)     *
 * ========================================================================== */
static void test_close_semantics(void) {
  ring_buffer_t *rb = rb_create(2, 16);
  assert(rb != NULL);
  uint32_t epoch = rb_epoch(rb);

  uint32_t v1 = 10, v2 = 20;
  assert(rb_push(rb, &v1, sizeof(v1), 1, epoch) == 0);
  assert(rb_push(rb, &v2, sizeof(v2), 2, epoch) == 0);

  /* Block a pusher on the full buffer, then close the buffer */
  block_push_arg_t ba = {.rb = rb,
                         .epoch = epoch,
                         .push_rc = 0,
                         .completed = false,
                         .mtx = PTHREAD_MUTEX_INITIALIZER};
  pthread_t tid;
  assert(pthread_create(&tid, NULL, blocking_pusher_thread, &ba) == 0);

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
  nanosleep(&ts, NULL);

  rb_close(rb);
  pthread_join(tid, NULL);
  assert(ba.push_rc == -1);

  /* Pop must still drain the 2 already-queued items before returning -1 */
  uint32_t out = 0, len = 0, tag = 0;
  assert(rb_pop(rb, &out, &len, &tag) == 1);
  assert(tag == 1 && out == 10);
  assert(rb_pop(rb, &out, &len, &tag) == 1);
  assert(tag == 2 && out == 20);

  /* Once closed AND empty, rb_pop returns -1 */
  assert(rb_pop(rb, &out, &len, &tag) == -1);

  pthread_mutex_destroy(&ba.mtx);
  rb_destroy(rb);
}

/* ========================================================================== *
 * TEST 5: 1,000,000-Item Producer/Consumer Stress Test with Checksum         *
 * ========================================================================== */
#define STRESS_ITEMS 1000000U

typedef struct {
  ring_buffer_t *rb;
  uint32_t epoch;
  uint64_t checksum;
} stress_ctx_t;

static inline uint32_t mix32(uint32_t i) {
  uint32_t x = i * 2654435761U;
  return x ^ (x >> 16);
}

static void *stress_producer(void *arg) {
  stress_ctx_t *ctx = (stress_ctx_t *)arg;
  uint64_t sum = 0;

  for (uint32_t i = 1; i <= STRESS_ITEMS; i++) {
    uint32_t val = mix32(i);
    sum += val;
    int rc = rb_push(ctx->rb, &val, sizeof(val), i, ctx->epoch);
    assert(rc == 0);
  }

  /* Push final EOF marker */
  assert(rb_push(ctx->rb, NULL, 0, RB_TAG_END, ctx->epoch) == 0);
  ctx->checksum = sum;
  return NULL;
}

static void *stress_consumer(void *arg) {
  stress_ctx_t *ctx = (stress_ctx_t *)arg;
  uint64_t sum = 0;
  uint32_t expected_seq = 1;

  for (;;) {
    uint32_t val = 0, len = 0, tag = 0;
    int rc = rb_pop(ctx->rb, &val, &len, &tag);
    if (rc == 0) {
      /* Non-blocking pop found empty buffer; yield briefly */
      sched_yield();
      continue;
    }
    assert(rc == 1);
    if (tag == RB_TAG_END) {
      assert(len == 0);
      break;
    }
    assert(tag == expected_seq);
    assert(len == sizeof(uint32_t));
    assert(val == mix32(expected_seq));
    sum += val;
    expected_seq++;
  }

  assert(expected_seq == STRESS_ITEMS + 1U);
  ctx->checksum = sum;
  return NULL;
}

static void test_1m_items_stress_checksum(void) {
  ring_buffer_t *rb = rb_create(RB_DEFAULT_SLOTS, sizeof(uint32_t));
  assert(rb != NULL);

  stress_ctx_t prod_ctx = {.rb = rb, .epoch = rb_epoch(rb), .checksum = 0};
  stress_ctx_t cons_ctx = {.rb = rb, .epoch = rb_epoch(rb), .checksum = 1};

  pthread_t ptid, ctid;
  assert(pthread_create(&ptid, NULL, stress_producer, &prod_ctx) == 0);
  assert(pthread_create(&ctid, NULL, stress_consumer, &cons_ctx) == 0);

  pthread_join(ptid, NULL);
  pthread_join(ctid, NULL);

  assert(prod_ctx.checksum == cons_ctx.checksum);
  rb_destroy(rb);
}

int main(void) {
  printf(
      "====================================================================\n");
  printf("PodFlow Session 2+3 — Ring Buffer Test Suite (Member 2)\n");
  printf(
      "====================================================================\n");

  RUN_TEST(test_single_thread_wrap_around);
  RUN_TEST(test_blocking_push_released_by_pop);
  RUN_TEST(test_flush_and_stale_epoch_discard);
  RUN_TEST(test_close_semantics);
  RUN_TEST(test_1m_items_stress_checksum);

  printf(
      "====================================================================\n");
  printf("ALL 5 RING BUFFER TEST SUITES PASSED SUCCESSFULLY.\n");
  printf(
      "====================================================================\n");
  return 0;
}