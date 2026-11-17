/*
 * pacing_experiment.c - measures pacer lateness (owner: Member 1)
 *
 * usage: ./pacing_experiment [ticks]        (default 600 ticks = 30 s per scenario)
 *
 * Scenarios, each running `ticks` ticks of 50 ms with a simulated 2 ms of
 * "send work" per tick:
 *   1. absolute pacer, idle machine
 *   2. absolute pacer, 50  busy (CPU-spinning) threads
 *   3. absolute pacer, 200 busy threads
 *   4. naive relative sleep (work, then nanosleep(50 ms)), idle: the drift demo
 *
 * lateness_k = wake-up time of tick k  -  (t0 + k * 50 ms)   [microseconds]
 * It is measured here with its own clock reads, independent of pacer_t's
 * internal statistics (the last column cross-checks them).
 * Wall time per run is about ticks * 50 ms * 4 scenarios.
 */
#include "pacing.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_SEC    1000000000LL
#define INTERVAL_NS   50000000L      /* 50 ms                      */
#define WORK_NS       2000000L       /* simulated send work: 2 ms  */
#define DEFAULT_TICKS 600            /* 30 s                       */

static atomic_int  g_stop;
static atomic_uint g_sink;           /* keeps the busy loop from being optimised away */

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

static void sleep_ns(long long ns)
{
    struct timespec t = { (time_t)(ns / NS_PER_SEC), (long)(ns % NS_PER_SEC) };
    while (nanosleep(&t, &t) == -1 && errno == EINTR)
        ;                            /* t now holds the remaining time */
}

static void *busy_thread(void *arg)
{
    unsigned x = 1;
    (void)arg;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        for (int i = 0; i < 10000; i++)
            x = x * 1664525u + 1013904223u;
        atomic_store_explicit(&g_sink, x, memory_order_relaxed);
    }
    return NULL;
}

static int cmp_ll(const void *a, const void *b)
{
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

static void run_scenario(const char *name, int ticks, int nbusy, int naive)
{
    pthread_t *th = nbusy > 0 ? calloc((size_t)nbusy, sizeof *th) : NULL;
    long long *late = malloc((size_t)ticks * sizeof *late);
    long long *sorted = malloc((size_t)ticks * sizeof *sorted);
    int started = 0;
    pacer_t p;

    if (!late || !sorted || (nbusy > 0 && !th)) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    atomic_store(&g_stop, 0);
    for (; started < nbusy; started++) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 64 * 1024);
        int e = pthread_create(&th[started], &at, busy_thread, NULL);
        pthread_attr_destroy(&at);
        if (e != 0) {
            fprintf(stderr, "  (only %d of %d busy threads could start)\n", started, nbusy);
            break;
        }
    }
    if (started > 0)
        sleep_ns(300000000LL);       /* let the spinners actually get on a CPU */

    long long t0 = now_ns();
    pacer_init(&p, INTERVAL_NS);     /* anchor is a few ns after t0 */

    for (int k = 0; k < ticks; k++) {
        if (naive) {
            late[k] = now_ns() - (t0 + (long long)k * INTERVAL_NS);
            sleep_ns(WORK_NS);               /* the "send"                     */
            sleep_ns(INTERVAL_NS);           /* relative: error accumulates    */
        } else {
            pacer_wait_next(&p);
            late[k] = now_ns() - (t0 + (long long)k * INTERVAL_NS);
            sleep_ns(WORK_NS);               /* the "send"                     */
        }
    }

    atomic_store(&g_stop, 1);
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);

    long long sum = 0, mn = late[0], mx = late[0];
    for (int k = 0; k < ticks; k++) {
        sum += late[k];
        if (late[k] < mn) mn = late[k];
        if (late[k] > mx) mx = late[k];
        sorted[k] = late[k];
    }
    qsort(sorted, (size_t)ticks, sizeof *sorted, cmp_ll);
    long long p99 = sorted[(size_t)((double)(ticks - 1) * 0.99)];

    printf("%-28s %5d %9.1f %9.1f %9.1f %10.1f %11.1f",
           name, started,
           (double)mn / 1e3, (double)sum / ticks / 1e3, (double)p99 / 1e3,
           (double)mx / 1e3, (double)late[ticks - 1] / 1e3);
    if (naive)
        printf(" %7s %9s\n", "-", "-");
    else
        printf(" %7llu %9.1f\n", (unsigned long long)p.late_resets,
               (double)p.lateness_max_ns / 1e3);

    free(th);
    free(late);
    free(sorted);
}

int main(int argc, char **argv)
{
    int ticks = DEFAULT_TICKS;

    if (argc > 2 || (argc == 2 && (ticks = atoi(argv[1])) < 10)) {
        fprintf(stderr, "usage: %s [ticks>=10]\n", argv[0]);
        return 2;
    }

    printf("pacing_experiment: %d ticks x 50 ms (%.1f s ideal) per scenario, "
           "%ld CPU(s) online\n", ticks, ticks * 0.05, sysconf(_SC_NPROCESSORS_ONLN));
    printf("lateness in microseconds; 'last' = lateness of the final tick "
           "(= accumulated drift)\n\n");
    printf("%-28s %5s %9s %9s %9s %10s %11s %7s %9s\n",
           "scenario", "busy", "min_us", "avg_us", "p99_us", "max_us", "last_us",
           "resets", "pacerMax");

    run_scenario("absolute, idle",           ticks,   0, 0);
    run_scenario("absolute, 50 busy threads", ticks,  50, 0);
    run_scenario("absolute, 200 busy threads", ticks, 200, 0);
    run_scenario("naive relative sleep, idle", ticks,   0, 1);
    return 0;
}