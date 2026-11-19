/**
 * @file pacing.h
 * @brief Absolute-time pacer (one chunk per 50 ms) for the PodFlow sender loop.
 *
 * Owner: Member 1 (Socket Core, Pacing, Server). Frozen API: contract v1.1.
 *
 * Timeline model: tick k is due at  t_anchor + k * interval_ns, where
 * t_anchor is the CLOCK_MONOTONIC time of pacer_init()/pacer_reset().
 * Tick 0 is due immediately, so the first chunk goes out at once after a
 * (re)start, resume or seek. Every deadline is computed from the previous
 * DEADLINE, never from "now", so wake-up latency and send time cannot add up
 * to drift.
 *
 * Typical use inside the connection thread (see server.c):
 *
 *     pacer_reset(&p);                        // PLAY / resume / seek
 *     ...
 *     pacer_timeout(&p, &ts);                 // ppoll() timeout (0 if due)
 *     if (due && session_next(...) == 1) {    // there is a chunk to send
 *         send_frame(...);
 *         pacer_tick_done(&p);                // only after a real send
 *     }                                       // underrun: do NOT call it
 *
 * Not thread-safe: one pacer_t belongs to exactly one thread.
 */
#ifndef PODFLOW_PACING_H
#define PODFLOW_PACING_H

#include <stdint.h>
#include <time.h>

/** 50 ms = one chunk of 8820 bytes at 176400 bytes/s. */
#define PACING_DEFAULT_INTERVAL_NS 50000000L

/** Late-reset rule: lateness above this many intervals re-anchors the schedule. */
#define PACING_MAX_LATE_TICKS 5

typedef struct {
    struct timespec next_tick;      /* absolute CLOCK_MONOTONIC deadline of the pending tick */
    long            interval_ns;    /* distance between ticks (nanoseconds)                   */
    uint64_t        ticks;          /* completed ticks (pacer_tick_done calls)                */
    uint64_t        late_resets;    /* times the late-reset rule re-anchored the schedule     */
    long long       lateness_sum_ns;/* sum of per-tick lateness                               */
    long long       lateness_max_ns;/* worst per-tick lateness                                */
} pacer_t;

/** Start a schedule now. interval_ns <= 0 selects the 50 ms default. Zeroes the stats. */
void pacer_init(pacer_t *p, long interval_ns);

/** Re-anchor: next_tick = now (tick due immediately). Statistics are kept. */
void pacer_reset(pacer_t *p);

/**
 * Time left until the pending tick, clamped to >= 0, written to *out
 * (suitable as a ppoll() timeout).
 * @return 1 if the tick is due now (*out = 0), 0 otherwise.
 */
int pacer_timeout(const pacer_t *p, struct timespec *out);

/**
 * Call right after the chunk of the pending tick was sent.
 * lateness = now - next_tick (>= 0) is recorded; for the server this includes the
 * send time, i.e. "how late the chunk was completed". Then next_tick += interval.
 * Late-reset rule: if lateness > PACING_MAX_LATE_TICKS * interval, next_tick is
 * re-anchored to now + interval and late_resets++ (never a burst, never a skip).
 */
void pacer_tick_done(pacer_t *p);

/**
 * Blocking variant: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) until the
 * pending tick (retrying on EINTR), then pacer_tick_done(). Used by the
 * experiment; the server waits with ppoll() instead so it can also read commands.
 */
void pacer_wait_next(pacer_t *p);

#endif /* PODFLOW_PACING_H */