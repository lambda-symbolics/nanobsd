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
#include <sys/sysctl.h>
#include <sys/lpsched.h>

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

uint64_t	lpsched_st_considered;
uint64_t	lpsched_st_exempt_class;
uint64_t	lpsched_st_exempt_fg;
uint64_t	lpsched_st_exempt_estcpu;
uint64_t	lpsched_st_pack_sibling;
uint64_t	lpsched_st_pack_shallow;
uint64_t	lpsched_st_pack_deep;
uint64_t	lpsched_st_pack_busy;
uint64_t	lpsched_st_pack_none;
uint64_t	lpsched_st_pack_held;
uint64_t	lpsched_st_catch_held;
uint64_t	lpsched_st_coal_applied;
uint64_t	lpsched_st_coal_short;
uint64_t	lpsched_st_coal_precise;

static char	lpsched_stats_buf[512];

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

	snprintf(lpsched_stats_buf, sizeof(lpsched_stats_buf),
	    "considered=%" PRIu64 " exempt_class=%" PRIu64
	    " exempt_fg=%" PRIu64 " exempt_estcpu=%" PRIu64
	    " pack_sibling=%" PRIu64 " pack_shallow=%" PRIu64
	    " pack_deep=%" PRIu64 " pack_busy=%" PRIu64
	    " pack_none=%" PRIu64 " pack_held=%" PRIu64
	    " catch_held=%" PRIu64 " coal_applied=%" PRIu64
	    " coal_short=%" PRIu64 " coal_precise=%" PRIu64,
	    lpsched_st_considered, lpsched_st_exempt_class,
	    lpsched_st_exempt_fg, lpsched_st_exempt_estcpu,
	    lpsched_st_pack_sibling, lpsched_st_pack_shallow,
	    lpsched_st_pack_deep, lpsched_st_pack_busy,
	    lpsched_st_pack_none, lpsched_st_pack_held,
	    lpsched_st_catch_held, lpsched_st_coal_applied,
	    lpsched_st_coal_short, lpsched_st_coal_precise);
	node.sysctl_data = lpsched_stats_buf;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
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
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "stats",
	    SYSCTL_DESCR("packing/exemption/coalescing counters"),
	    lpsched_sysctl_stats, 0, NULL, sizeof(lpsched_stats_buf),
	    CTL_CREATE, CTL_EOL);
}
