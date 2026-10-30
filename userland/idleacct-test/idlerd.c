#include <sys/types.h>
#include <sys/sysctl.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	uint64_t a[65], b[65];
	size_t la = sizeof(a), lb = sizeof(b);
	int ms = argc > 1 ? atoi(argv[1]) : 2000, n, c;

	if (sysctlbyname("machdep.lpsched.cpu_idle", a, &la, NULL, 0) == -1)
		err(1, "cpu_idle");
	usleep(ms * 1000);
	if (sysctlbyname("machdep.lpsched.cpu_idle", b, &lb, NULL, 0) == -1)
		err(1, "cpu_idle");
	n = (int)(la / 8) - 1;
	printf("dt %llu us:", (unsigned long long)(b[0] - a[0]));
	for (c = 1; c <= n; c++)
		printf(" cpu%d %.3f", c - 1,
		    1.0 - (double)(b[c] - a[c]) / (double)(b[0] - a[0]));
	printf("\n");
	return 0;
}
