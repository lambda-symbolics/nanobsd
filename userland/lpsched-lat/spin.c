/* CPU-bound counter: prints iterations per second on SIGTERM. */
#include <stdio.h>
#include <signal.h>
#include <time.h>
static volatile sig_atomic_t done;
static void on_term(int s) { (void)s; done = 1; }
int
main(void)
{
	struct timespec a, b;
	unsigned long long n = 0;
	volatile double x = 1.0;
	signal(SIGTERM, on_term);
	clock_gettime(CLOCK_MONOTONIC, &a);
	while (!done) { for (int i = 0; i < 1000; i++) x = x * 1.0000001 + 0.5; n++; }
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("spin %.0f kiter/s\n", n / ((b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9) );
	return 0;
}
