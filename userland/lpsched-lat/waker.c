/* Periodic waker: sleeps to absolute 10 ms deadlines, reports lateness. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t done;
static void on_term(int s) { (void)s; done = 1; }

int
main(int argc, char **argv)
{
	struct timespec next, now;
	long period_ns = 10000000, late, max = 0, n = 0, sum = 0, over1 = 0;
	long hist[8] = { 0 };	/* <50us <100 <200 <500 <1ms <2ms <5ms more */
	int b;

	(void)argc; (void)argv;
	signal(SIGTERM, on_term);
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!done) {
		next.tv_nsec += period_ns;
		if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0 && !done)
			;
		clock_gettime(CLOCK_MONOTONIC, &now);
		late = (now.tv_sec - next.tv_sec) * 1000000L + (now.tv_nsec - next.tv_nsec) / 1000;
		if (late < 0) late = 0;
		b = late < 50 ? 0 : late < 100 ? 1 : late < 200 ? 2 : late < 500 ? 3 :
		    late < 1000 ? 4 : late < 2000 ? 5 : late < 5000 ? 6 : 7;
		hist[b]++; n++; sum += late; if (late > max) max = late; if (late >= 1000) over1++;
		/* a little work per period */
		for (volatile int i = 0; i < 20000; i++) ;
	}
	printf("waker n=%ld mean=%ldus max=%ldus >=1ms=%ld hist(<50,<100,<200,<500,<1m,<2m,<5m,more)=%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
	    n, n ? sum / n : 0, max, over1, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
	return 0;
}
