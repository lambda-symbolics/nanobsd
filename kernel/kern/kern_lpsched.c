/*	$NetBSD$	*/

/*
 * lpsched - LISPBSD laptop power scheduler: state, counters and sysctls.
 *
 * The mechanisms live in kern_runq.c (packing), kern_timeout.c (callout
 * coalescing), wskbd.c/wsmouse.c (input timestamp) and the cidle module
 * (idle depth).  This file only owns the knobs:
 *
 *	machdep.lpsched.enabled		master switch (0 = stock behaviour)
 *	machdep.lpsched.pack		0 off, 1 mild, 2 aggressive
 *	machdep.lpsched.coalesce_ms	callout slack grid, 0 off
 *	machdep.lpsched.fgpid		focused PID (exempt from packing)
 *	machdep.lpsched.estcpu_thresh	0 = off, else interactive if estcpu>>11
 *					is below it (SCHED_4BSD only)
 *	machdep.lpsched.idle_ms		ms since last keyboard/mouse input (RO)
 *	machdep.lpsched.stats		placement/exemption counters (RO)
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/inttypes.h>
#include <sys/cpu.h>
#include <sys/sysctl.h>
#include <sys/lwp.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/bitops.h>
#include <sys/lpsched.h>

#include <machine/cpu_counter.h>

int	lpsched_enabled = 0;
int	lpsched_pack = 0;
int	lpsched_coalesce_ms = 0;
int	lpsched_fgpid = 0;
/*
 * Default off.  A threshold of 2 exempts every LWP with estcpu < 4096, which
 * is exactly the low-duty-cycle background work packing is meant to gather,
 * so it suppressed the mechanism it was supposed to refine.  It is also
 * SCHED_4BSD-only: SCHED_M2 never updates l_estcpu, so under M2 a non-zero
 * threshold exempts everything.  Set it deliberately when A/B testing.
 */
int	lpsched_estcpu_thresh = 0;

volatile int		lpsched_last_input;
struct lpsched_cpu	lpsched_cpu[MAXCPUS] __cacheline_aligned;

int	lpsched_stats_enabled = 1;
int	lpsched_latency = 1;
int	lpsched_wait_budget_us = 2000;

static uint64_t	lpsched_tsc_mhz;	/* cpu_counter() ticks per us */

static const char * const lpsched_class_name[LPSCHED_NCLASS] = {
	[LPSCHED_CL_FG]		= "fg",
	[LPSCHED_CL_USER]	= "user",
	[LPSCHED_CL_KERNEL]	= "kernel",
	[LPSCHED_CL_RT]		= "rt",
};

static const char * const lpsched_stat_name[LPSCHED_NSTAT] = {
	[LPSCHED_ST_CONSIDERED]		= "considered",
	[LPSCHED_ST_EXEMPT_CLASS]	= "exempt_class",
	[LPSCHED_ST_EXEMPT_FG]		= "exempt_fg",
	[LPSCHED_ST_EXEMPT_ESTCPU]	= "exempt_estcpu",
	[LPSCHED_ST_PACK_SIBLING]	= "pack_sibling",
	[LPSCHED_ST_PACK_SHALLOW]	= "pack_shallow",
	[LPSCHED_ST_PACK_DEEP]		= "pack_deep",
	[LPSCHED_ST_PACK_BUSY]		= "pack_busy",
	[LPSCHED_ST_PACK_NONE]		= "pack_none",
	[LPSCHED_ST_PACK_HELD]		= "pack_held",
	[LPSCHED_ST_CATCH_HELD]		= "catch_held",
	[LPSCHED_ST_COAL_APPLIED]	= "coal_applied",
	[LPSCHED_ST_COAL_NOSLACK]	= "coal_noslack",
	[LPSCHED_ST_COAL_SHORT]		= "coal_short",
	[LPSCHED_ST_COAL_PRECISE]	= "coal_precise",
	[LPSCHED_ST_PACK_GUARD]		= "pack_guard",
	[LPSCHED_ST_PACK_LONGBURST]	= "pack_longburst",
	[LPSCHED_ST_CATCH_ESCAPE]	= "catch_escape",
};

/*
 * Bump a counter on the running CPU.  Deliberately not atomic: the value is
 * diagnostic, it is only ever written by its own CPU, and an atomic here
 * would reintroduce the cross-CPU traffic the per-CPU layout removes.
 */
void
lpsched_stat_bump(u_int idx)
{
	u_int i;

	if (__predict_false(idx >= LPSCHED_NSTAT))
		return;
	i = cpu_index(curcpu());
	if (__predict_true(i < MAXCPUS))
		lpsched_cpu[i].st[idx]++;
}

/*
 * Demand and latency accounting.  Timestamps are cpu_counter() (the
 * invariant TSC, synchronized across CPUs here); converted to microseconds
 * only when a wait or a burst ends.
 */
static inline uint64_t
lpsched_us(uint64_t from, uint64_t to)
{

	if (__predict_false(lpsched_tsc_mhz == 0))
		lpsched_tsc_mhz = MAX(1, cpu_frequency(curcpu()) / 1000000);
	return to > from ? (to - from) / lpsched_tsc_mhz : 0;
}

static inline u_int
lpsched_class(const struct lwp *l)
{

	if (l->l_class != SCHED_OTHER)
		return LPSCHED_CL_RT;
	if ((l->l_flag & LW_SYSTEM) != 0 || l->l_proc == &proc0)
		return LPSCHED_CL_KERNEL;
	if (lpsched_fgpid != 0 && l->l_proc->p_pid == lpsched_fgpid)
		return LPSCHED_CL_FG;
	return LPSCHED_CL_USER;
}

/* Moving average with weight 1/4 for the new sample. */
static inline uint32_t
lpsched_ewma(uint32_t avg, uint64_t sample)
{

	if (sample > UINT32_MAX)
		sample = UINT32_MAX;
	return avg == 0 ? (uint32_t)sample :
	    avg - avg / 4 + (uint32_t)(sample / 4);
}

/*
 * sched_enqueue(): l was put on ci's run queue (spc_mutex held).  A wait
 * starts unless one is already running (a priority change dequeues and
 * requeues; a migration moves it), and an empty queue now has work.
 */
void
lpsched_enqueued(struct lwp *l, struct cpu_info *ci, bool was_empty)
{
	uint64_t now = cpu_counter();
	u_int i = cpu_index(ci);

	if (l->l_lp_runnable == 0)
		l->l_lp_runnable = now;
	if (was_empty && i < MAXCPUS)
		lpsched_cpu[i].q_since = now;
}

/* sched_dequeue(): ci's run queue may have emptied (spc_mutex held). */
void
lpsched_dequeued(struct cpu_info *ci, bool now_empty)
{
	u_int i = cpu_index(ci);

	if (now_empty && i < MAXCPUS)
		lpsched_cpu[i].q_since = 0;
}

/*
 * sched_nextlwp() on this CPU: prev's burst ends, next (NULL: the idle
 * LWP) gets the CPU after waiting since it became runnable.
 */
void
lpsched_switch(struct lwp *prev, struct lwp *next)
{
	struct lpsched_cpu *pc;
	uint64_t now = cpu_counter(), us;
	u_int i = cpu_index(curcpu()), cl, b;

	if (__predict_false(i >= MAXCPUS))
		return;
	pc = &lpsched_cpu[i];

	if (prev != NULL && (prev->l_flag & LW_IDLE) == 0 &&
	    prev->l_lp_oncpu != 0) {
		us = lpsched_us(prev->l_lp_oncpu, now);
		prev->l_lp_burst_us = lpsched_ewma(prev->l_lp_burst_us, us);
		pc->run_us[lpsched_class(prev)] += us;
		prev->l_lp_oncpu = 0;
	}
	if (next == NULL) {
		pc->critical = 0;
		pc->cur_burst_us = 0;
		return;
	}

	cl = lpsched_class(next);
	if (next->l_lp_runnable != 0) {
		us = lpsched_us(next->l_lp_runnable, now);
		/*
		 * A stamp left over from before the accounting was turned
		 * off and on again is no wait: drop anything over a minute.
		 */
		if (us < 60 * 1000000) {
			b = (us >> 3) == 0 ? 0 : fls64(us >> 3);
			pc->lat[cl][MIN(b, LPSCHED_NBUCKET - 1)]++;
			pc->wait_us[cl] += us;
			if (us > pc->wait_max_us[cl])
				pc->wait_max_us[cl] = (uint32_t)us;
			next->l_lp_wait_us = lpsched_ewma(next->l_lp_wait_us,
			    us);
		}
		next->l_lp_runnable = 0;
	}
	next->l_lp_oncpu = now;
	pc->critical = (cl == LPSCHED_CL_FG || cl == LPSCHED_CL_RT);
	pc->cur_burst_us = next->l_lp_burst_us;
}

/* Has ci's run queue held work for longer than the wait budget? */
bool
lpsched_queue_overdue(struct cpu_info *ci)
{
	u_int i = cpu_index(ci);
	uint64_t since;

	if (i >= MAXCPUS)
		return false;
	since = lpsched_cpu[i].q_since;
	return since != 0 &&
	    lpsched_us(since, cpu_counter()) > (uint64_t)lpsched_wait_budget_us;
}

static char	lpsched_stats_buf[768];
static char	lpsched_latency_buf[2048];

/*
 * Generic clamped integer handler: rnode->sysctl_data points at the
 * variable, [lo, hi] is the accepted range.
 */
static int
lpsched_sysctl_clamp(SYSCTLFN_ARGS, int lo, int hi)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = *(int *)rnode->sysctl_data;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		return error;
	if (val < lo || val > hi)
		return EINVAL;
	*(int *)rnode->sysctl_data = val;
	return 0;
}

static int
lpsched_sysctl_enabled(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 1);
}

static int
lpsched_sysctl_pack(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 2);
}

static int
lpsched_sysctl_coalesce(SYSCTLFN_ARGS)
{

	/* 200 ms is already 20 ticks at hz=100; anything more is a bug. */
	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 200);
}

static int
lpsched_sysctl_fgpid(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, INT_MAX);
}

static int
lpsched_sysctl_estcpu(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 64);
}

static int
lpsched_sysctl_idle_ms(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int idle, val;

	idle = getticks() - lpsched_last_input;
	if (idle < 0)
		idle = 0;
	/* 64-bit intermediate: ticks * 1000 overflows int after ~25 days. */
	val = (int)MIN((int64_t)idle * 1000 / hz, INT_MAX);
	node.sysctl_data = &val;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int
lpsched_sysctl_stats(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	uint64_t total[LPSCHED_NSTAT];
	size_t len = 0;
	u_int i, c;

	memset(total, 0, sizeof(total));
	for (c = 0; c < MAXCPUS; c++)
		for (i = 0; i < LPSCHED_NSTAT; i++)
			total[i] += lpsched_cpu[c].st[i];

	for (i = 0; i < LPSCHED_NSTAT && len < sizeof(lpsched_stats_buf); i++) {
		len += snprintf(lpsched_stats_buf + len,
		    sizeof(lpsched_stats_buf) - len, "%s%s=%" PRIu64,
		    (i == 0) ? "" : " ", lpsched_stat_name[i], total[i]);
	}
	node.sysctl_data = lpsched_stats_buf;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int
lpsched_sysctl_latency(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 1);
}

static int
lpsched_sysctl_budget(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 100, 1000000);
}

/*
 * Per class: waits counted, mean and max wait, the 50/90/99th percentile
 * as bucket upper bounds, run time, and the histogram (bucket i = waits
 * under 8 us << i, the last one longer).
 */
static int
lpsched_sysctl_latency_hist(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	uint64_t hist[LPSCHED_NBUCKET], n, wait, run, acc;
	uint32_t max;
	size_t len = 0;
	u_int cl, c, b, p, pct[3] = { 50, 90, 99 };

	for (cl = 0; cl < LPSCHED_NCLASS; cl++) {
		memset(hist, 0, sizeof(hist));
		wait = run = 0;
		max = 0;
		for (c = 0; c < MAXCPUS; c++) {
			for (b = 0; b < LPSCHED_NBUCKET; b++)
				hist[b] += lpsched_cpu[c].lat[cl][b];
			wait += lpsched_cpu[c].wait_us[cl];
			run += lpsched_cpu[c].run_us[cl];
			max = MAX(max, lpsched_cpu[c].wait_max_us[cl]);
		}
		for (n = 0, b = 0; b < LPSCHED_NBUCKET; b++)
			n += hist[b];
		len += snprintf(lpsched_latency_buf + len,
		    sizeof(lpsched_latency_buf) - len,
		    "%s%s: n=%" PRIu64 " mean=%" PRIu64 "us max=%uus",
		    cl == 0 ? "" : "\n", lpsched_class_name[cl], n,
		    n ? wait / n : 0, max);
		for (p = 0; p < 3 && n != 0 &&
		    len < sizeof(lpsched_latency_buf); p++) {
			for (acc = 0, b = 0; b < LPSCHED_NBUCKET - 1; b++) {
				acc += hist[b];
				if (n != 0 && acc * 100 >= n * pct[p])
					break;
			}
			len += snprintf(lpsched_latency_buf + len,
			    sizeof(lpsched_latency_buf) - len,
			    b == LPSCHED_NBUCKET - 1 ? " p%u>%uus" : " p%u<%uus",
			    pct[p], b == LPSCHED_NBUCKET - 1 ?
			    8u << (LPSCHED_NBUCKET - 2) : 8u << b);
		}
		if (len < sizeof(lpsched_latency_buf))
			len += snprintf(lpsched_latency_buf + len,
			    sizeof(lpsched_latency_buf) - len,
			    " run=%" PRIu64 "ms hist=", run / 1000);
		for (b = 0; b < LPSCHED_NBUCKET &&
		    len < sizeof(lpsched_latency_buf); b++)
			len += snprintf(lpsched_latency_buf + len,
			    sizeof(lpsched_latency_buf) - len, "%s%" PRIu64,
			    b == 0 ? "" : ",", hist[b]);
		if (len >= sizeof(lpsched_latency_buf))
			break;
	}
	if (len >= sizeof(lpsched_latency_buf))
		lpsched_latency_buf[sizeof(lpsched_latency_buf) - 1] = '\0';
	node.sysctl_data = lpsched_latency_buf;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

/* Writing anything clears the latency histograms (not the counters). */
static int
lpsched_sysctl_latency_reset(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val = 0;
	u_int c;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		return error;
	for (c = 0; c < MAXCPUS; c++) {
		memset(lpsched_cpu[c].lat, 0, sizeof(lpsched_cpu[c].lat));
		memset(lpsched_cpu[c].wait_us, 0,
		    sizeof(lpsched_cpu[c].wait_us));
		memset(lpsched_cpu[c].run_us, 0, sizeof(lpsched_cpu[c].run_us));
		memset(lpsched_cpu[c].wait_max_us, 0,
		    sizeof(lpsched_cpu[c].wait_max_us));
	}
	return 0;
}

static int
lpsched_sysctl_stats_enabled(SYSCTLFN_ARGS)
{

	return lpsched_sysctl_clamp(SYSCTLFN_CALL(rnode), 0, 1);
}

SYSCTL_SETUP(sysctl_lpsched_setup, "sysctl machdep.lpsched subtree setup")
{
	const struct sysctlnode *node = NULL;

	/* Usual idiom: make sure the top-level node exists, ignore EEXIST. */
	sysctl_createv(clog, 0, NULL, NULL, CTLFLAG_PERMANENT, CTLTYPE_NODE,
	    "machdep", NULL, NULL, 0, NULL, 0, CTL_MACHDEP, CTL_EOL);

	sysctl_createv(clog, 0, NULL, &node, CTLFLAG_PERMANENT, CTLTYPE_NODE,
	    "lpsched", SYSCTL_DESCR("LISPBSD laptop power scheduler"),
	    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_CREATE, CTL_EOL);
	if (node == NULL)
		return;

	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "enabled",
	    SYSCTL_DESCR("master switch; 0 = stock scheduler/callout behaviour"),
	    lpsched_sysctl_enabled, 0, &lpsched_enabled, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "pack",
	    SYSCTL_DESCR("idle-aware packing: 0 off, 1 mild, 2 aggressive"),
	    lpsched_sysctl_pack, 0, &lpsched_pack, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "coalesce_ms",
	    SYSCTL_DESCR("callout coalescing slack grid in ms, 0 off"),
	    lpsched_sysctl_coalesce, 0, &lpsched_coalesce_ms, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "fgpid",
	    SYSCTL_DESCR("focused process id, exempt from packing (0 none)"),
	    lpsched_sysctl_fgpid, 0, &lpsched_fgpid, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "estcpu_thresh",
	    SYSCTL_DESCR("0=off; else estcpu>>11 below this is interactive (4BSD)"),
	    lpsched_sysctl_estcpu, 0, &lpsched_estcpu_thresh, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_INT, "idle_ms",
	    SYSCTL_DESCR("milliseconds since the last keyboard/mouse input"),
	    lpsched_sysctl_idle_ms, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "stats_enabled",
	    SYSCTL_DESCR("count placement/coalescing events (0 to measure their cost)"),
	    lpsched_sysctl_stats_enabled, 0, &lpsched_stats_enabled, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "latency",
	    SYSCTL_DESCR("wake-to-run and burst accounting (0/1)"),
	    lpsched_sysctl_latency, 0, &lpsched_latency, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "wait_budget_us",
	    SYSCTL_DESCR("queueing a packed LWP may cost, us"),
	    lpsched_sysctl_budget, 0, &lpsched_wait_budget_us, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "latency_hist",
	    SYSCTL_DESCR("wake-to-run waits per class"),
	    lpsched_sysctl_latency_hist, 0, NULL, sizeof(lpsched_latency_buf),
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "latency_reset",
	    SYSCTL_DESCR("write to clear the latency histograms"),
	    lpsched_sysctl_latency_reset, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "stats",
	    SYSCTL_DESCR("packing/exemption/coalescing counters"),
	    lpsched_sysctl_stats, 0, NULL, sizeof(lpsched_stats_buf),
	    CTL_CREATE, CTL_EOL);
}
