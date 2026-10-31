/* lwptime PID SECS: exact CPU per thread (kinfo_lwp l_rtime) over SECS */
#include <sys/param.h>
#include <sys/sysctl.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct snap { int n; struct kinfo_lwp *l; };

static struct snap
take(int pid)
{
	int mib[5] = { CTL_KERN, KERN_LWP, pid, sizeof(struct kinfo_lwp), 0 };
	size_t len = 0;
	struct snap s;

	if (sysctl(mib, 5, NULL, &len, NULL, 0) == -1) err(1, "sysctl");
	len += 64 * sizeof(struct kinfo_lwp);
	s.l = malloc(len);
	mib[4] = len / sizeof(struct kinfo_lwp);
	if (sysctl(mib, 5, s.l, &len, NULL, 0) == -1) err(1, "sysctl");
	s.n = len / sizeof(struct kinfo_lwp);
	return s;
}

int
main(int argc, char **argv)
{
	int pid, secs, i, j;

	if (argc < 2) {
		fprintf(stderr, "usage: lwptime PID [SECS]\n");
		return 2;
	}
	pid = atoi(argv[1]);
	secs = argc > 2 ? atoi(argv[2]) : 5;
	struct snap a = take(pid), b;
	double tot = 0;

	sleep(secs);
	b = take(pid);
	for (i = 0; i < b.n; i++) {
		double t1 = b.l[i].l_rtime_sec + b.l[i].l_rtime_usec / 1e6, t0 = 0;
		for (j = 0; j < a.n; j++)
			if (a.l[j].l_lid == b.l[i].l_lid)
				t0 = a.l[j].l_rtime_sec + a.l[j].l_rtime_usec / 1e6;
		if (t1 - t0 > 0.0005 * secs)
			printf("%6.2f%% %6d %s\n", 100 * (t1 - t0) / secs, b.l[i].l_lid, b.l[i].l_name);
		tot += t1 - t0;
	}
	printf("%6.2f%% total\n", 100 * tot / secs);
	return 0;
}
