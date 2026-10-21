/*
 * lpschedd - LISPBSD laptop power governor.
 *
 * Userland policy for the kernel's machdep.lpsched mechanisms plus the HWP
 * energy/performance preference.  The kernel stays policy-free; this daemon
 * picks a profile and writes the knobs.
 *
 * Inputs
 *   AC state    /var/run/lpsched.ac ("1"/"0"), written by powerd's acadapter
 *               hook; seeded from envstat(8) at startup.
 *   busy        CPU utilisation since the previous wakeup (kern.cp_time),
 *               with a hysteresis band.  The 1-minute load average used
 *               before followed a change in work a minute late.
 *   idle_ms     machdep.lpsched.idle_ms, ms since the last keyboard/mouse input.
 *   fgpid       "fgpid <pid>" datagrams on /var/run/lpsched.sock from the WM.
 *   input       "input <pid>" datagrams from a compositor (Mahogany) that sees
 *               every input event: sent when input arrives after a quiet
 *               spell.  While that compositor lives, input needs no polling,
 *               so the daemon wakes every POLL_SLOW_MS instead of 4 times a
 *               second (the kernel only recomputes the load every 5 s).
 *
 * Outputs
 *   machdep.hwp.epp, machdep.lpsched.{enabled,pack,coalesce_ms,fgpid}
 *   machdep.hwp.pl1 (RAPL long-term package limit): lowered on battery to
 *   pl1_bat (-P, mW; 0 leaves it alone), the boot value on AC and at exit.
 *   Bursts (PL2) are untouched, so short work stays quick and only sustained
 *   work (a build left running, a busy background page) is capped.  The
 *   value is re-checked every PL_CHECK_MS, as firmware may reset it at a
 *   resume or a charger change.
 *
 * Profiles
 *   perf        AC present: EPP perf, no packing, no coalescing.
 *   balanced    battery + input within the last boost_ms (-b, 1 s): the
 *               interaction boost.  It used to last the whole 10 s
 *               interactive window, holding EPP 128 long after the input
 *               that justified it.
 *   load        battery + busy + input within the interactive window.
 *   batch       battery + busy, no recent input.
 *   powersave   battery, not busy, no input within boost_ms.
 * Heavy load on battery no longer means EPP perf.  Measured on the X1 Nano
 * (docs/epp-load.org): a parallel build at EPP 32 or 128 took 25 s and the
 * most energy; 192 took 12 % longer for 11 % less, 224 took 47 % longer for
 * 27 % less, whole-machine energy over the same window.  EPP 32 was no
 * faster than 128 at all.
 * Upshifts (toward perf) apply immediately; downshifts wait until the
 * condition has held for DOWNSHIFT_MS (elapsed time, not a poll count).
 *
 * Build: cc -O2 -Wall -o lpschedd lpschedd.c -lutil
 */

#include <sys/types.h>
#include <sys/sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

#define	AC_FILE		"/var/run/lpsched.ac"
#define	EPP_FILE	"/var/run/lpsched.epp0"	/* EPP found by the first instance */
#define	SOCK_PATH	"/var/run/lpsched.sock"

#define	DOWNSHIFT_MS		500	/* how long a lower profile must persist */
#define	PL_CHECK_MS		30000	/* re-check the battery PL1 this often */
#define	POLL_ACTIVE_MS		250	/* poll period in perf/balanced */
#define	POLL_IDLE_MS		500	/* poll period in powersave */
#define	POLL_SLOW_MS		5000	/* while a compositor reports input */

/* Ordered from most to least performance: a lower value is an upshift. */
enum profile { P_PERF = 0, P_BALANCED, P_LOAD, P_BATCH, P_POWERSAVE, P_COUNT };

static const struct {
	const char *name;
	int epp, pack, coalesce_ms;
} profiles[P_COUNT] = {
	[P_PERF]      = { "perf",       32, 0,  0 },
	[P_BALANCED]  = { "balanced",  128, 1, 20 },
	[P_LOAD]      = { "load",      192, 0,  0 },
	[P_BATCH]     = { "batch",     224, 0,  0 },
	[P_POWERSAVE] = { "powersave", 240, 2, 50 },
};

static int	 interactive_ms = 10000;	/* -i: input within this = interactive */
static int	 boost_ms = 1000;		/* -b: EPP 128 this long after input */
static double	 load_hi = 0.25;		/* -H: CPU busy fraction = load/batch */
static double	 load_lo = 0.15;		/* -L: below this releases the latch */
static int	 pl1_bat = 10000;		/* -P: battery PL1, mW (0: leave) */
static int	 pl1_boot = 0;			/* machdep.hwp.pl1_boot */
static bool	 debug = false;			/* -d: foreground, log to stderr */
static bool	 dryrun = false;		/* -n: don't write sysctls */
static int	 sockfd = -1;
static int	 compositor_pid = 0;		/* sends "input" datagrams */
static int	 orig_epp = 128;
static volatile sig_atomic_t quit = 0;

static void
logmsg(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (debug) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else
		vsyslog(pri, fmt, ap);
	va_end(ap);
}

static int
sysctl_get(const char *name, int *val)
{
	size_t len = sizeof(*val);

	if (sysctlbyname(name, val, &len, NULL, 0) == -1) {
		logmsg(LOG_WARNING, "sysctl %s: %s", name, strerror(errno));
		return -1;
	}
	return 0;
}

static int
sysctl_set(const char *name, int val)
{

	if (debug)
		logmsg(LOG_DEBUG, "  %s=%d%s", name, val, dryrun ? " (dry)" : "");
	if (dryrun)
		return 0;
	if (sysctlbyname(name, NULL, NULL, &val, sizeof(val)) == -1) {
		logmsg(LOG_WARNING, "sysctl %s=%d: %s", name, val,
		    strerror(errno));
		return -1;
	}
	return 0;
}

/* AC state from powerd's file; -1 if unknown. */
static int
read_ac_file(void)
{
	char buf[8];
	int fd, n;

	fd = open(AC_FILE, O_RDONLY);
	if (fd == -1)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	return buf[0] == '1';
}

/* One-shot seed from envstat(8) so we know the state before powerd fires. */
static int
seed_ac_envstat(void)
{
	FILE *fp;
	char line[256];
	int ac = -1;

	fp = popen("/usr/sbin/envstat -d acpiacad0 2>/dev/null", "r");
	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "connected") == NULL)
			continue;
		ac = (strstr(line, "TRUE") != NULL || strstr(line, "ON") != NULL);
	}
	pclose(fp);
	return ac;
}

static void
write_ac_file(int ac)
{
	FILE *fp;

	fp = fopen(AC_FILE, "w");
	if (fp == NULL)
		return;
	fprintf(fp, "%d\n", ac);
	fclose(fp);
}

static int
read_epp_file(int *val)
{
	FILE *fp;
	int rv;

	fp = fopen(EPP_FILE, "r");
	if (fp == NULL)
		return -1;
	rv = (fscanf(fp, "%d", val) == 1) ? 0 : -1;
	fclose(fp);
	return rv;
}

static void
write_epp_file(int val)
{
	FILE *fp;

	fp = fopen(EPP_FILE, "w");
	if (fp == NULL)
		return;
	fprintf(fp, "%d\n", val);
	fclose(fp);
}

static void
apply_profile(enum profile p)
{

	logmsg(LOG_INFO, "profile -> %s (epp %d, pack %d, coalesce %d ms)",
	    profiles[p].name, profiles[p].epp, profiles[p].pack,
	    profiles[p].coalesce_ms);
	/* Order matters on downshift: coalesce before pack, both before EPP. */
	sysctl_set("machdep.lpsched.coalesce_ms", profiles[p].coalesce_ms);
	sysctl_set("machdep.lpsched.pack", profiles[p].pack);
	sysctl_set("machdep.hwp.epp", profiles[p].epp);
}

static enum profile
decide(int ac, double busy, int idle_ms, bool *load_latched)
{

	if (ac != 0)			/* AC, or unknown: do no harm */
		return P_PERF;
	if (*load_latched && busy < load_lo)
		*load_latched = false;
	else if (!*load_latched && busy > load_hi)
		*load_latched = true;
	if (idle_ms < boost_ms)
		return P_BALANCED;
	if (*load_latched)
		return idle_ms < interactive_ms ? P_LOAD : P_BATCH;
	return P_POWERSAVE;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Busy fraction of all CPUs since the previous call (0 on the first). */
static double
cpu_busy(void)
{
	static uint64_t last[CPUSTATES];
	static bool have;
	uint64_t cur[CPUSTATES], total = 0, idle;
	size_t len = sizeof(cur);
	double busy = 0.0;
	int i;

	if (sysctlbyname("kern.cp_time", cur, &len, NULL, 0) == -1)
		return 0.0;
	if (have) {
		for (i = 0; i < CPUSTATES; i++)
			total += cur[i] - last[i];
		idle = cur[CP_IDLE] - last[CP_IDLE];
		if (total > 0)
			busy = 1.0 - (double)idle / (double)total;
	}
	memcpy(last, cur, sizeof(last));
	have = true;
	return busy;
}

/* The PL1 for this supply; re-applied when it drifted (resume, firmware). */
static void
apply_pl1(int ac)
{
	int want, cur;

	if (pl1_bat <= 0 || pl1_boot <= 0)
		return;
	want = ac == 0 ? (pl1_bat < pl1_boot ? pl1_bat : pl1_boot) : pl1_boot;
	if (sysctl_get("machdep.hwp.pl1", &cur) == -1)
		return;
	/* The register holds 1/8 W steps: allow for the rounding. */
	if (cur > want - 125 && cur < want + 125)
		return;
	logmsg(LOG_INFO, "package PL1 %d -> %d mW (%s)", cur, want,
	    ac == 0 ? "battery" : "AC");
	sysctl_set("machdep.hwp.pl1", want);
}

static int
open_socket(void)
{
	struct sockaddr_un sun;
	int fd;

	fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (fd == -1) {
		logmsg(LOG_ERR, "socket: %s", strerror(errno));
		return -1;
	}
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	strlcpy(sun.sun_path, SOCK_PATH, sizeof(sun.sun_path));
	unlink(SOCK_PATH);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		logmsg(LOG_ERR, "bind %s: %s", SOCK_PATH, strerror(errno));
		close(fd);
		return -1;
	}
	chmod(SOCK_PATH, 0666);		/* the WM runs unprivileged */
	return fd;
}

static void
handle_socket(int *fgpid)
{
	char buf[64];
	ssize_t n;
	int pid;

	n = recv(sockfd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return;
	buf[n] = '\0';
	if (sscanf(buf, "input %d", &pid) == 1 && pid > 0) {
		/* The main loop re-reads idle_ms right after this wakeup. */
		if (pid != compositor_pid)
			logmsg(LOG_INFO, "compositor %d reports input: polling "
			    "every %d ms", pid, POLL_SLOW_MS);
		compositor_pid = pid;
		return;
	}
	if (sscanf(buf, "fgpid %d", &pid) != 1 || pid < 0)
		return;
	if (pid != *fgpid) {
		*fgpid = pid;
		sysctl_set("machdep.lpsched.fgpid", pid);
	}
}

/*
 * How long to sleep.  Without a compositor reporting input, poll for it.
 * With one, wake only to confirm a downshift, when the interactive window
 * runs out, or every POLL_SLOW_MS for load and AC changes.
 */
static int
poll_timeout(enum profile cur, int pending, int idle_ms)
{
	int left;

	if (compositor_pid == 0)
		return cur == P_POWERSAVE ? POLL_IDLE_MS : POLL_ACTIVE_MS;
	if (pending > 0)
		return POLL_IDLE_MS;
	/* Wake when the boost or the interactive window runs out. */
	if (cur == P_BALANCED) {
		left = boost_ms - idle_ms + 50;
		if (left < 50)
			left = 50;
		return left < POLL_SLOW_MS ? left : POLL_SLOW_MS;
	}
	if (cur == P_LOAD) {
		left = interactive_ms - idle_ms + 100;
		if (left < POLL_ACTIVE_MS)
			left = POLL_ACTIVE_MS;
		return left < POLL_SLOW_MS ? left : POLL_SLOW_MS;
	}
	/* While busy, measure often enough to see the work end. */
	if (cur == P_BATCH)
		return POLL_ACTIVE_MS * 4;
	return POLL_SLOW_MS;
}

static void
on_signal(int sig)
{

	quit = 1;
}

static void
cleanup(void)
{

	logmsg(LOG_INFO, "exiting: restoring stock behaviour, epp %d", orig_epp);
	sysctl_set("machdep.lpsched.enabled", 0);
	sysctl_set("machdep.lpsched.pack", 0);
	sysctl_set("machdep.lpsched.coalesce_ms", 0);
	sysctl_set("machdep.lpsched.fgpid", 0);
	sysctl_set("machdep.hwp.epp", orig_epp);
	if (pl1_bat > 0 && pl1_boot > 0)
		sysctl_set("machdep.hwp.pl1", pl1_boot);
	if (!dryrun)
		unlink(EPP_FILE);
	if (sockfd != -1)
		close(sockfd);
	unlink(SOCK_PATH);
}

static void
usage(void)
{

	fprintf(stderr, "usage: lpschedd [-dn] [-b boost_ms] [-i interactive_ms] "
	    "[-H busy_hi] [-L busy_lo] [-P pl1_bat_mw]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	struct pollfd pfd;
	enum profile cur = P_COUNT, want;
	bool load_latched = false;
	int ac, idle_ms = 0, fgpid = 0, pending = 0, c, timeout, last_ac = -2;
	uint64_t pending_since = 0, pl_checked = 0;
	double load;

	while ((c = getopt(argc, argv, "dnb:i:H:L:P:")) != -1) {
		switch (c) {
		case 'd': debug = true; break;
		case 'n': dryrun = true; break;
		case 'b': boost_ms = atoi(optarg); break;
		case 'P': pl1_bat = atoi(optarg); break;
		case 'i': interactive_ms = atoi(optarg); break;
		case 'H': load_hi = atof(optarg); break;
		case 'L': load_lo = atof(optarg); break;
		default: usage();
		}
	}

	if (!debug) {
		openlog("lpschedd", LOG_PID, LOG_DAEMON);
		if (daemon(0, 0) == -1) {
			perror("daemon");
			return 1;
		}
	}
	pidfile(NULL);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, SIG_IGN);

	/*
	 * Remember the EPP we found.  A restart while a profile is active
	 * must not adopt our own setting as the "original"; the first
	 * instance's value lives in EPP_FILE until a clean exit removes it
	 * (/var/run is cleared at boot, when sysctl.conf sets EPP anyway).
	 */
	if (read_epp_file(&orig_epp) == -1) {
		if (sysctl_get("machdep.hwp.epp", &orig_epp) == -1)
			orig_epp = 128;
		write_epp_file(orig_epp);
	}

	ac = read_ac_file();
	if (ac == -1) {
		ac = seed_ac_envstat();
		if (ac != -1)
			write_ac_file(ac);
	}
	if (pl1_bat > 0 && sysctl_get("machdep.hwp.pl1_boot", &pl1_boot) == -1)
		pl1_boot = 0;		/* hwp without RAPL: leave PL1 alone */
	logmsg(LOG_INFO, "started: ac=%d boost=%dms interactive=%dms "
	    "busy_hi=%.2f busy_lo=%.2f epp(orig)=%d pl1 boot=%d bat=%d mW",
	    ac, boost_ms, interactive_ms, load_hi, load_lo, orig_epp,
	    pl1_boot, pl1_bat);
	(void)cpu_busy();

	sockfd = open_socket();
	sysctl_set("machdep.lpsched.enabled", 1);

	while (!quit) {
		pfd.fd = sockfd;
		pfd.events = POLLIN;
		timeout = poll_timeout(cur, pending, idle_ms);
		if (poll(&pfd, sockfd == -1 ? 0 : 1, timeout) > 0 &&
		    (pfd.revents & POLLIN))
			handle_socket(&fgpid);

		/*
		 * The WM only reports focus gains, so the last PID goes stale
		 * when that window closes; drop it once the process is gone.
		 */
		if (fgpid != 0 && kill(fgpid, 0) == -1 && errno == ESRCH) {
			fgpid = 0;
			sysctl_set("machdep.lpsched.fgpid", 0);
		}
		if (compositor_pid != 0 && kill(compositor_pid, 0) == -1 &&
		    errno == ESRCH) {
			logmsg(LOG_INFO, "compositor %d gone: polling input "
			    "again", compositor_pid);
			compositor_pid = 0;
		}

		c = read_ac_file();
		if (c != -1)
			ac = c;
		load = cpu_busy();
		if (sysctl_get("machdep.lpsched.idle_ms", &idle_ms) == -1)
			idle_ms = 0;
		if (ac != last_ac || now_ms() - pl_checked >= PL_CHECK_MS) {
			apply_pl1(ac);
			last_ac = ac;
			pl_checked = now_ms();
		}

		want = decide(ac, load, idle_ms, &load_latched);
		if (cur == P_COUNT || want < cur) {
			/* First pass, or upshift: immediate. */
			cur = want;
			pending = 0;
			apply_profile(cur);
		} else if (want > cur) {
			if (pending == 0) {
				pending = 1;
				pending_since = now_ms();
			} else if (now_ms() - pending_since >= DOWNSHIFT_MS) {
				cur = want;
				pending = 0;
				apply_profile(cur);
			}
		} else
			pending = 0;
	}

	cleanup();
	return 0;
}
