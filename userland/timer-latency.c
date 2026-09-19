/* Measure relative nanosleep latency against CLOCK_MONOTONIC.
 * Build: cc -O2 -Wall -Wextra -Werror -o timer-latency timer-latency.c
 * Run:   schedctl -A 0 ./timer-latency 500 50
 * Arguments: sample count, requested sleep in milliseconds.
 */
#include <sys/types.h>
#include <errno.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double
now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
		err(1, "clock_gettime");
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static long
argument(const char *text, long limit)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || *end != '\0' || value < 1 || value > limit)
		errx(1, "invalid positive argument: %s (maximum %ld)", text, limit);
	return value;
}

static int
compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int
main(int argc, char **argv)
{
	struct timespec request, remaining;
	double *samples, start, before, sum = 0;
	long count, ms, i, late100 = 0;

	if (argc != 3)
		errx(1, "usage: %s samples milliseconds", argv[0]);
	count = argument(argv[1], 1000000);
	ms = argument(argv[2], 60000);
	samples = calloc((size_t)count, sizeof(*samples));
	if (samples == NULL)
		err(1, "calloc");
	request.tv_sec = ms / 1000;
	request.tv_nsec = (ms % 1000) * 1000000;
	start = now_ms();
	for (i = 0; i < count; i++) {
		before = now_ms();
		remaining = request;
		while (nanosleep(&remaining, &remaining) == -1)
			if (errno != EINTR)
				err(1, "nanosleep");
		samples[i] = now_ms() - before;
		sum += samples[i];
		if (samples[i] > ms + 100)
			late100++;
	}
	qsort(samples, (size_t)count, sizeof(*samples), compare);
	printf("pid=%ld n=%ld request=%ldms elapsed=%.3fs "
	    "min=%.3f mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3fms "
	    "over100ms=%ld\n", (long)getpid(), count, ms,
	    (now_ms() - start) / 1000.0, samples[0], sum / count,
	    samples[(count - 1) * 50 / 100], samples[(count - 1) * 95 / 100],
	    samples[(count - 1) * 99 / 100], samples[count - 1], late100);
	free(samples);
	return 0;
}
