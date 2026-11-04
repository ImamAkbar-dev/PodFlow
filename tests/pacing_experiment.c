/*
 * pacing_experiment.c - measures pacer lateness (owner: Member 1)
 *
 * usage: ./pacing_experiment [ticks] [naive]
 *   ticks  number of 50 ms ticks (default 600 = 30 s)
 *   naive  use relative nanosleep() instead of the absolute pacer (comparison)
 */
#include "pacing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_PER_SEC 1000000000LL
#define WORK_NS    2000000L          /* simulated per-chunk work: 2 ms */

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

static void sleep_ns(long ns)
{
    struct timespec t = { ns / NS_PER_SEC, ns % NS_PER_SEC };
    nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
    long ticks = 600;
    int naive = 0;

    if (argc >= 2) {
        char *end = NULL;
        ticks = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || ticks < 1) {
            fprintf(stderr, "usage: %s [ticks] [naive]\n", argv[0]);
            return 2;
        }
    }
    if (argc >= 3 && strcmp(argv[2], "naive") == 0)
        naive = 1;

    const long interval = PACING_DEFAULT_INTERVAL_NS;
    long long min_us = -1, max_us = 0, sum_us = 0, over_1ms = 0;
    pacer_t p;

    printf("mode: %s, %ld ticks of %ld ms (expected total %.3f s)\n",
           naive ? "NAIVE relative sleep" : "ABSOLUTE pacer", ticks,
           interval / 1000000L, (double)ticks * interval / 1e9);

    long long start = now_ns();
    pacer_init(&p, interval);

    for (long i = 1; i <= ticks; i++) {
        long long late_ns;

        if (naive) {
            sleep_ns(interval);                          /* relative sleep */
            late_ns = now_ns() - (start + i * (long long)interval);
            if (late_ns < 0)
                late_ns = 0;
        } else {
            late_ns = pacer_wait_next(&p);
        }

        long long late_us = late_ns / 1000;
        if (min_us < 0 || late_us < min_us) min_us = late_us;
        if (late_us > max_us) max_us = late_us;
        sum_us += late_us;
        if (late_us > 1000) over_1ms++;

        sleep_ns(WORK_NS);                               /* simulate sending a chunk */
    }

    double total = (double)(now_ns() - start) / 1e9;
    printf("lateness (us): min=%lld avg=%.1f max=%lld\n",
           min_us, (double)sum_us / (double)ticks, max_us);
    printf("ticks later than 1 ms: %lld of %ld\n", over_1ms, ticks);
    printf("total time: %.3f s (ideal %.3f s, drift %+.3f s)\n", total,
           (double)ticks * interval / 1e9,
           total - (double)ticks * interval / 1e9);
    return 0;
}