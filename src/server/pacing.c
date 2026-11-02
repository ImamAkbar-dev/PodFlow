#include "pacing.h"

#include <errno.h>

#define NS_PER_SEC 1000000000L

/* t += ns (ns may be larger than one second). */
static void ts_add_ns(struct timespec *t, long ns)
{
    t->tv_sec  += ns / NS_PER_SEC;
    t->tv_nsec += ns % NS_PER_SEC;
    if (t->tv_nsec >= NS_PER_SEC) {
        t->tv_sec++;
        t->tv_nsec -= NS_PER_SEC;
    }
}

/* a - b in nanoseconds. */
static long long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (long long)(a->tv_sec - b->tv_sec) * NS_PER_SEC +
           (long long)(a->tv_nsec - b->tv_nsec);
}

void pacer_init(pacer_t *p, long interval_ns)
{
    p->interval_ns = interval_ns > 0 ? interval_ns : PACING_DEFAULT_INTERVAL_NS;
    clock_gettime(CLOCK_MONOTONIC, &p->next_tick);
}

void pacer_reset(pacer_t *p)
{
    clock_gettime(CLOCK_MONOTONIC, &p->next_tick);
}

long pacer_wait_next(pacer_t *p)
{
    int rc;
    struct timespec now;

    ts_add_ns(&p->next_tick, p->interval_ns);   /* absolute: no drift */

    /* clock_nanosleep returns the error number directly (it does not use
     * errno). With TIMER_ABSTIME, retrying after EINTR is safe because the
     * target time is absolute, so no remaining-time bookkeeping is needed. */
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &p->next_tick, NULL);
    } while (rc == EINTR);

    clock_gettime(CLOCK_MONOTONIC, &now);
    long long late = ts_diff_ns(&now, &p->next_tick);
    if (late < 0)
        late = 0;

    /* Far behind: re-anchor so we do not fire a burst of back-to-back ticks. */
    if (late > (long long)PACING_MAX_CATCHUP_TICKS * p->interval_ns)
        p->next_tick = now;

    return (long)late;
}