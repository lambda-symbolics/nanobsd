/*
 * burst: a fixed amount of CPU work in short bursts, like page scripts
 * reacting to events: COUNT bursts of ITERS xorshift steps, one every
 * PERIOD_MS.  Prints each burst's duration in ms, then mean and p95.
 *   burst COUNT ITERS PERIOD_MS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int
cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int
main(int argc, char **argv)
{
	int count = argc > 1 ? atoi(argv[1]) : 60;
	long iters = argc > 2 ? atol(argv[2]) : 100000000L;
	int period = argc > 3 ? atoi(argv[3]) : 2000;
	double *d = calloc(count, sizeof *d), start = now_ms(), sum = 0;
	volatile uint64_t sink;
	uint64_t x = 88172645463325252ULL;

	for (int i = 0; i < count; i++) {
		double t0 = now_ms();
		for (long j = 0; j < iters; j++) {
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		}
		sink = x;
		d[i] = now_ms() - t0;
		sum += d[i];
		/* Absolute deadlines: the period does not drift with the work. */
		double next = start + (i + 1) * (double)period;
		struct timespec ts = { (time_t)(next / 1e3),
		    (long)((next - (time_t)(next / 1e3) * 1e3) * 1e6) };
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0)
			;
	}
	(void)sink;
	qsort(d, count, sizeof *d, cmp);
	printf("bursts=%d mean_ms=%.1f p95_ms=%.1f max_ms=%.1f\n", count,
	    sum / count, d[(int)(count * 0.95)], d[count - 1]);
	return 0;
}
