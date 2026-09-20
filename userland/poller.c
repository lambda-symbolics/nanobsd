/* Low-duty-cycle background poller: ~1ms of work every 20ms (~5% duty).
   This is the workload class lpsched packing targets; a static idle box
   has nothing to pack, so it cannot show the mechanism working. */
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	volatile double x = 0;
	long secs = (argc > 1) ? atol(argv[1]) : 120;
	long iters = secs * 50, i, j;
	for (i = 0; i < iters; i++) {
		for (j = 0; j < 60000; j++) x += j * 0.5;
		nanosleep(&ts, NULL);
	}
	return 0;
}
