/*
 * poller - a repeatable low-duty-cycle background task for lpsched testing.
 *
 * A static idle machine has nothing to pack, so it cannot show the packing
 * policy working either way.  This provides the workload class the policy
 * targets, with properties a benchmark needs:
 *
 *   - ABSOLUTE deadlines.  An earlier version did fixed work then a relative
 *     nanosleep(), so its period stretched by however long the work and the
 *     scheduler took: neither its phase nor its duty cycle were held constant
 *     across the configurations being compared, which is precisely what a
 *     scheduler comparison must not vary.
 *   - fixed compute per batch, and a report of batches COMPLETED plus
 *     deadlines MISSED, so a run that quietly did less work is visible rather
 *     than being read as a power win.
 *   - a settable period, so the workload can be placed above or below the
 *     coalescing grid on purpose instead of by accident.
 *   - a settable start phase, to stagger or align instances deliberately.
 *   - on SIGUSR1 it writes its running counts to -f <file>, so a benchmark
 *     can take per-configuration DELTAS at each measurement boundary.  A
 *     single total at the end of an A/B/A/B run also covers the settling
 *     intervals and the tail after the last sample, so it cannot show that
 *     the configurations did equal work.  Writing only on demand keeps the
 *     workload's own I/O out of the samples.
 *
 * usage: poller [-p period_ms] [-w work_iters] [-d duration_s] [-s phase_ms]
 *               [-f statusfile]
 * prints: "batches=N missed=M period_ms=P"
 */

#include <sys/time.h>

#include <err.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t report_now;

static void
on_usr1(int sig)
{

	report_now = 1;
}

/* True when "a" is strictly after "b". */
static int
ts_after(const struct timespec *a, const struct timespec *b)
{

	return a->tv_sec > b->tv_sec ||
	    (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void
ts_add_ms(struct timespec *ts, long ms)
{

	ts->tv_nsec += (ms % 1000) * 1000000L;
	ts->tv_sec += ms / 1000;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec++;
	}
}

/* Sleep until the absolute deadline, tolerating the absence of TIMER_ABSTIME. */
static void
sleep_until(const struct timespec *deadline)
{
	struct timespec now, delta;

#ifdef TIMER_ABSTIME
	if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline,
	    NULL) == 0)
		return;
#endif
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	delta.tv_sec = deadline->tv_sec - now.tv_sec;
	delta.tv_nsec = deadline->tv_nsec - now.tv_nsec;
	if (delta.tv_nsec < 0) {
		delta.tv_nsec += 1000000000L;
		delta.tv_sec--;
	}
	if (delta.tv_sec < 0)
		return;			/* already late */
	nanosleep(&delta, NULL);
}

int
main(int argc, char **argv)
{
	struct timespec next, now, end;
	long period_ms = 200, work = 60000, duration_s = 120, phase_ms = 0;
	unsigned long batches = 0, missed = 0;
	const char *statusfile = NULL;
	volatile double x = 0;
	long i;
	int c;

	while ((c = getopt(argc, argv, "p:w:d:s:f:")) != -1) {
		switch (c) {
		case 'p': period_ms = atol(optarg); break;
		case 'w': work = atol(optarg); break;
		case 'd': duration_s = atol(optarg); break;
		case 's': phase_ms = atol(optarg); break;
		case 'f': statusfile = optarg; break;
		default:
			errx(1, "usage: poller [-p period_ms] [-w work_iters] "
			    "[-d duration_s] [-s phase_ms] [-f statusfile]");
		}
	}
	signal(SIGUSR1, on_usr1);
	if (period_ms <= 0)
		errx(1, "period must be positive");

	if (clock_gettime(CLOCK_MONOTONIC, &next) != 0)
		err(1, "clock_gettime");
	end = next;
	end.tv_sec += duration_s;
	if (phase_ms > 0)
		ts_add_ms(&next, phase_ms);

	for (;;) {
		sleep_until(&next);
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			break;
		if (now.tv_sec > end.tv_sec ||
		    (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
			break;

		for (i = 0; i < work; i++)
			x += i * 0.5;
		batches++;

		/*
		 * Advance along the ORIGINAL timeline by whole periods until
		 * the deadline is in the future, counting each release that
		 * was skipped.  Resetting to "now + period" instead would move
		 * the phase by however long the scheduler delayed us, which is
		 * exactly the variable under test.
		 */
		ts_add_ms(&next, period_ms);
		if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
			while (ts_after(&now, &next)) {
				missed++;
				ts_add_ms(&next, period_ms);
			}
		}

		if (report_now && statusfile != NULL) {
			FILE *fp;

			report_now = 0;
			if ((fp = fopen(statusfile, "w")) != NULL) {
				fprintf(fp, "batches=%lu missed=%lu\n",
				    batches, missed);
				fclose(fp);
			}
		}
	}

	printf("batches=%lu missed=%lu period_ms=%ld\n", batches, missed,
	    period_ms);
	return 0;
}
