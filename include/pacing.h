#ifndef PACING_H
#define PACING_H

#include <time.h>

/* 50 ms = one chunk of 8820 bytes at 176400 bytes/s */
#define PACING_DEFAULT_INTERVAL_NS 50000000L

/* If we fall more than this many intervals behind (e.g. after a PAUSE or a
 * long stall), re-anchor the schedule instead of sending a burst to "catch up". */
#define PACING_MAX_CATCHUP_TICKS 10

typedef struct {
    struct timespec next_tick;   /* absolute CLOCK_MONOTONIC time of the next tick */
    long            interval_ns; /* distance between ticks, in nanoseconds         */
} pacer_t;

/* Start a schedule "now". interval_ns <= 0 selects the 50 ms default.
 * The first pacer_wait_next() returns one interval after this call. */
void pacer_init(pacer_t *p, long interval_ns);

/* Sleep until the next absolute tick (next_tick += interval_ns).
 * Returns how late we woke up, in nanoseconds (>= 0). Callers may ignore it. */
long pacer_wait_next(pacer_t *p);

/* Re-anchor the schedule to "now" (use after PAUSE/SEEK in Session 2). */
void pacer_reset(pacer_t *p);

#endif /* PACING_H */