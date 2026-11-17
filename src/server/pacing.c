/**
 * @file pacing.c
 * @brief Absolute-time pacer implementation (see include/pacing.h).
 */
#include "pacing.h"

#include <errno.h>

#define NS_PER_SEC 1000000000LL

/* t += ns. ns may be negative or larger than one second. */
static void ts_add_ns(struct timespec *t, long long ns)
{
    long long total = (long long)t->tv_nsec + ns;

    t->tv_sec += (time_t)(total / NS_PER_SEC);
    total %= NS_PER_SEC;
    if (total < 0) {                 /* C division truncates toward zero */
        total += NS_PER_SEC;
        t->tv_sec--;
    }
    t->tv_nsec = (long)total;
}

/* a - b in nanoseconds. */
static long long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (long long)(a->tv_sec - b->tv_sec) * NS_PER_SEC +
           (long long)(a->tv_nsec - b->tv_nsec);
}

void pacer_reset(pacer_t *p)
{
    clock_gettime(CLOCK_MONOTONIC, &p->next_tick);
}

void pacer_init(pacer_t *p, long interval_ns)
{
    p->interval_ns      = interval_ns > 0 ? interval_ns : PACING_DEFAULT_INTERVAL_NS;
    p->ticks            = 0;
    p->late_resets      = 0;
    p->lateness_sum_ns  = 0;
    p->lateness_max_ns  = 0;
    pacer_reset(p);
}

int pacer_timeout(const pacer_t *p, struct timespec *out)
{
    struct timespec now;
    long long left;

    clock_gettime(CLOCK_MONOTONIC, &now);
    left = ts_diff_ns(&p->next_tick, &now);
    if (left <= 0) {
        out->tv_sec  = 0;
        out->tv_nsec = 0;
        return 1;
    }
    out->tv_sec  = (time_t)(left / NS_PER_SEC);
    out->tv_nsec = (long)(left % NS_PER_SEC);
    return 0;
}

void pacer_tick_done(pacer_t *p)
{
    struct timespec now;
    long long late;

    clock_gettime(CLOCK_MONOTONIC, &now);
    late = ts_diff_ns(&now, &p->next_tick);
    if (late < 0)
        late = 0;

    p->ticks++;
    p->lateness_sum_ns += late;
    if (late > p->lateness_max_ns)
        p->lateness_max_ns = late;

    /* Advance from the previous DEADLINE, not from "now": this is what makes
     * the schedule drift-free. */
    ts_add_ns(&p->next_tick, p->interval_ns);

    /* Far behind: re-anchor instead of sending a long burst to catch up.
     * The chunk just sent counts as "now", so the next one is one interval away. */
    if (late > (long long)PACING_MAX_LATE_TICKS * p->interval_ns) {
        p->next_tick = now;
        ts_add_ns(&p->next_tick, p->interval_ns);
        p->late_resets++;
    }
}

void pacer_wait_next(pacer_t *p)
{
    int rc;

    /* clock_nanosleep() returns the error number directly (errno is not set).
     * With TIMER_ABSTIME a retry after EINTR is safe: the target is absolute,
     * so no "remaining time" bookkeeping is needed. A deadline already in the
     * past returns immediately. */
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &p->next_tick, NULL);
    } while (rc == EINTR);

    pacer_tick_done(p);
}