/*
 * tickrate - measure the hardclock tick rate against the monotonic clock.
 *
 * The tickless idle path stops the periodic tick and later credits the time
 * back as whole hardclock ticks, so a defect there shows up as hardclock
 * running slow (or fast) against real time.  Shell-level sampling of
 * kern.hardclock_ticks quantises elapsed time to whole seconds, which is far
 * coarser than the error being looked for; this reads a nanosecond monotonic
 * clock either side instead.
 *
 * Run it PINNED to the CPU that owns hardclock:  schedctl -A 0 tickrate 120
 * Only the primary CPU advances hardclock_ticks, and the sysctl returns the
 * stored value without forcing that CPU to complete any deferred tickless
 * accounting.  Sampling from another CPU can therefore catch CPU 0 partway
 * through a tickless interval and count outstanding-but-not-yet-credited
 * time as if it were lost: 0.6 s outstanding at the endpoint reads as -1%
 * over a 60 s run.  Pinning forces CPU 0 out of idle to run the sampler.
 *
 * usage: tickrate [seconds]     (default 60)
 */

#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int
read_ticks(long long *out)
{
	int v;
	size_t len = sizeof(v);

	if (sysctlbyname("kern.hardclock_ticks", &v, &len, NULL, 0) == -1)
		return -1;
	*out = v;
	return 0;
}

int
main(int argc, char **argv)
{
	struct timespec t1, t2, deadline;
	long long k1, k2;
	double elapsed, rate, hz;
	int secs = (argc > 1) ? atoi(argv[1]) : 60;
	int mib[2] = { CTL_KERN, KERN_CLOCKRATE };
	struct clockinfo ci;
	size_t len = sizeof(ci);

	if (sysctl(mib, 2, &ci, &len, NULL, 0) == -1)
		err(1, "KERN_CLOCKRATE");
	hz = ci.hz;

	if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0 || read_ticks(&k1) != 0)
		err(1, "sample 1");
	deadline = t1;
	deadline.tv_sec += secs;
	/*
	 * clock_nanosleep(2) returns the error number directly, not -1 with
	 * errno.  Testing for -1 therefore never retried an interrupted
	 * sleep, it just fell through and measured a short interval.
	 */
	for (;;) {
		int e = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
		    &deadline, NULL);
		if (e == 0)
			break;
		if (e != EINTR)
			errx(1, "clock_nanosleep: %s", strerror(e));
	}
	if (clock_gettime(CLOCK_MONOTONIC, &t2) != 0 || read_ticks(&k2) != 0)
		err(1, "sample 2");

	elapsed = (t2.tv_sec - t1.tv_sec) +
	    (t2.tv_nsec - t1.tv_nsec) / 1e9;
	rate = (k2 - k1) / elapsed;
	printf("elapsed=%.6fs ticks=%lld rate=%.4f/s hz=%.0f error=%+.3f%%\n",
	    elapsed, k2 - k1, rate, hz, (rate - hz) / hz * 100.0);
	return 0;
}
