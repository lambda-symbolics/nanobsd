/*	$NetBSD$	*/

/*
 * lpsched - LISPBSD laptop power scheduler hooks.
 *
 * Policy-free kernel mechanisms driven by the userland governor lpschedd(8)
 * through the machdep.lpsched sysctl tree.  Everything is inert until
 * machdep.lpsched.enabled is set, so a stock kernel is one sysctl away.
 *
 *   - idle-aware packing bias in sched_takecpu()/sched_preempted()/
 *     sched_idle() (kern_runq.c): place background LWPs on the cheapest CPU
 *     to wake instead of scattering them, keep them there, and stop idle
 *     cores waking up to steal one stray LWP.
 *   - callout coalescing (kern_timeout.c): round long, non-precise callouts
 *     up to a slack grid on the GLOBAL tick epoch so timers fire in clusters
 *     and idle gaps get longer.
 *   - input-activity timestamp from wskbd/wsmouse, exported as idle_ms.
 *
 * The per-CPU idle depth is written by the cidle module's idle loop.
 */

#ifndef _SYS_LPSCHED_H_
#define _SYS_LPSCHED_H_

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/atomic.h>

/* lpsched_cpu[].depth values */
#define	LPSCHED_IDLE_ACTIVE	0	/* running, or depth unknown */
#define	LPSCHED_IDLE_SHALLOW	1	/* short MWAIT, periodic tick running */
#define	LPSCHED_IDLE_DEEP	2	/* tickless MWAIT, expensive to wake */

/*
 * sched_4bsd.c's ESTCPU_SHIFT, mirrored here because it is file-local.
 * Only meaningful under SCHED_4BSD: SCHED_M2 never maintains l_estcpu, so
 * it would read 0 for every LWP.  See lpsched_estcpu_thresh, which defaults
 * to 0 (test disabled) for exactly that reason.
 */
#define	LPSCHED_ESTCPU_SHIFT	11

/*
 * Per-CPU idle depth, one cache line each: a bare uint8_t[] puts every CPU's
 * byte in the same line, so each idle entry and exit would invalidate the
 * line for all the others -- contention introduced by the observer.
 */
struct lpsched_cpu {
	volatile uint8_t	depth;
} __aligned(COHERENCY_UNIT);

extern struct lpsched_cpu lpsched_cpu[MAXCPUS];

extern int	lpsched_enabled;	/* master switch */
extern int	lpsched_pack;		/* 0 off, 1 mild, 2 aggressive */
extern int	lpsched_coalesce_ms;	/* callout slack grid in ms, 0 off */
extern int	lpsched_fgpid;		/* focused process from the WM, 0 none */
extern int	lpsched_estcpu_thresh;	/* 0 = off; else estcpu>>11 below this
					   counts as interactive (4BSD only) */
extern volatile int	lpsched_last_input;	/* getticks() at last input */

/*
 * Counters.  Only ever touched while lpsched is enabled, so a disabled
 * kernel keeps stock behaviour and stock cache traffic.  Without these
 * there is no way to tell a mechanism that did nothing from one that never
 * ran: the first round of measurements could not distinguish them.
 */
extern uint64_t	lpsched_st_considered;	/* placement decisions seen */
extern uint64_t	lpsched_st_exempt_class;	/* not SCHED_OTHER */
extern uint64_t	lpsched_st_exempt_fg;		/* focused process */
extern uint64_t	lpsched_st_exempt_estcpu;	/* interactive by estcpu */
extern uint64_t	lpsched_st_pack_sibling;	/* onto an awake core's sibling */
extern uint64_t	lpsched_st_pack_shallow;	/* onto a shallow-idle CPU */
extern uint64_t	lpsched_st_pack_deep;		/* had to wake a deep CPU */
extern uint64_t	lpsched_st_pack_busy;		/* queued behind a runner */
extern uint64_t	lpsched_st_pack_none;		/* fell back to stock */
extern uint64_t	lpsched_st_pack_held;		/* migration away suppressed */
extern uint64_t	lpsched_st_catch_held;		/* steal held back */
extern uint64_t	lpsched_st_coal_applied;	/* callout delayed onto grid */
extern uint64_t	lpsched_st_coal_short;		/* too short to coalesce */
extern uint64_t	lpsched_st_coal_precise;	/* CALLOUT_PRECISE */

static inline void
lpsched_stat(uint64_t *counter)
{

	atomic_inc_64((volatile uint64_t *)counter);
}

static inline bool
lpsched_packing(void)
{

	return __predict_false(lpsched_enabled != 0 && lpsched_pack > 0);
}

static inline void
lpsched_input_activity(void)
{

	lpsched_last_input = getticks();
}

static inline void
lpsched_set_idle_depth(u_int cpuidx, uint8_t depth)
{

	/*
	 * Gated: when lpsched is off this must not add a single store to the
	 * idle path, or "disabled" is not a clean baseline to measure against.
	 */
	if (__predict_false(lpsched_enabled != 0) && cpuidx < MAXCPUS)
		lpsched_cpu[cpuidx].depth = depth;
}

static inline uint8_t
lpsched_idle_depth(u_int cpuidx)
{

	return (cpuidx < MAXCPUS) ? lpsched_cpu[cpuidx].depth :
	    LPSCHED_IDLE_ACTIVE;
}

#endif /* !_SYS_LPSCHED_H_ */
