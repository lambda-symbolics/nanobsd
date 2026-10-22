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
 *
 * Demand and latency accounting (machdep.lpsched.latency): when an LWP
 * becomes runnable, how long it waits for a CPU (a histogram per class:
 * focused process, other user, kernel, realtime), how long it then runs.
 * Updated lazily at enqueue and at the scheduler's pick, no sampling
 * interrupt.  The per-LWP burst average also steers placement: packing
 * queues work only behind short bursts, and a queue that has waited past
 * machdep.lpsched.wait_budget_us may be stolen from after all.
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

extern int	lpsched_enabled;	/* master switch */
extern int	lpsched_pack;		/* 0 off, 1 mild, 2 aggressive */
extern int	lpsched_coalesce_ms;	/* callout slack grid in ms, 0 off */
extern int	lpsched_fgpid;		/* focused process from the WM, 0 none */
extern int	lpsched_estcpu_thresh;	/* 0 = off; else estcpu>>11 below this
					   counts as interactive (4BSD only) */
extern volatile int	lpsched_last_input;	/* getticks() at last input */

/*
 * Counters.  Per-CPU and cache-line separated: a set of global counters
 * atomically incremented from every CPU adds exactly the cross-CPU shared
 * writes this code exists to avoid, and it does so in the power-saving
 * configuration only -- i.e. it would tax the arm under test.  Aggregated on
 * read.  lpsched_stats_enabled turns them off so their own cost can be
 * measured.
 */
enum {
	LPSCHED_ST_CONSIDERED,		/* exemption decisions made */
	LPSCHED_ST_EXEMPT_CLASS,	/* not SCHED_OTHER */
	LPSCHED_ST_EXEMPT_FG,		/* focused process */
	LPSCHED_ST_EXEMPT_ESTCPU,	/* interactive by estcpu */
	LPSCHED_ST_PACK_SIBLING,	/* onto an awake core's idle sibling */
	LPSCHED_ST_PACK_SHALLOW,	/* onto a shallow-idle CPU */
	LPSCHED_ST_PACK_DEEP,		/* had to wake a deep-idle CPU */
	LPSCHED_ST_PACK_BUSY,		/* queued behind a running LWP */
	LPSCHED_ST_PACK_NONE,		/* fell back to stock placement */
	LPSCHED_ST_PACK_HELD,		/* cross-core migration suppressed */
	LPSCHED_ST_CATCH_HELD,		/* steal of available work held back */
	LPSCHED_ST_COAL_APPLIED,	/* callout delayed onto the grid */
	LPSCHED_ST_COAL_NOSLACK,	/* callout did not opt in to slack */
	LPSCHED_ST_COAL_SHORT,		/* opted in but shorter than the grid */
	LPSCHED_ST_COAL_PRECISE,	/* CALLOUT_PRECISE */
	LPSCHED_ST_PACK_GUARD,		/* sibling skipped: focused/RT core */
	LPSCHED_ST_PACK_LONGBURST,	/* not queued behind a long burst */
	LPSCHED_ST_CATCH_ESCAPE,	/* queue waited too long: steal */
	LPSCHED_NSTAT
};

/* Latency accounting classes and histogram buckets. */
enum {
	LPSCHED_CL_FG,			/* the focused process */
	LPSCHED_CL_USER,		/* other user timeshare */
	LPSCHED_CL_KERNEL,		/* kernel threads */
	LPSCHED_CL_RT,			/* SCHED_FIFO/SCHED_RR */
	LPSCHED_NCLASS
};
#define	LPSCHED_NBUCKET		16	/* bucket i: wait < 8 us << i */

/*
 * Per-CPU idle depth and counters, one cache line group each: a bare
 * uint8_t[] put every CPU's byte in the same line, so each idle entry and
 * exit invalidated the line for all the others -- contention introduced by
 * the observer.
 */
struct lpsched_cpu {
	volatile uint8_t	depth;
	volatile uint8_t	critical;	/* runs focused/realtime work */
	volatile uint32_t	cur_burst_us;	/* burst average of the runner */
	volatile uint64_t	q_since;	/* cpu_counter() when the run
						   queue became non-empty */
	uint64_t		st[LPSCHED_NSTAT];
	/* written by this CPU only, at its own scheduler pick */
	uint64_t		lat[LPSCHED_NCLASS][LPSCHED_NBUCKET];
	uint64_t		wait_us[LPSCHED_NCLASS];
	uint64_t		run_us[LPSCHED_NCLASS];
	uint32_t		wait_max_us[LPSCHED_NCLASS];
} __aligned(COHERENCY_UNIT);

extern struct lpsched_cpu lpsched_cpu[MAXCPUS];
extern int	lpsched_stats_enabled;
extern int	lpsched_latency;	/* demand/latency accounting */
extern int	lpsched_wait_budget_us;	/* longest acceptable queueing */

struct lwp;
struct cpu_info;
void	lpsched_stat_bump(u_int);
void	lpsched_enqueued(struct lwp *, struct cpu_info *, bool);
void	lpsched_dequeued(struct cpu_info *, bool);
void	lpsched_switch(struct lwp *, struct lwp *);
bool	lpsched_queue_overdue(struct cpu_info *);

static inline bool
lpsched_accounting(void)
{

	return __predict_false(lpsched_enabled != 0 && lpsched_latency != 0);
}

static inline void
lpsched_stat(u_int idx)
{

	if (__predict_false(lpsched_stats_enabled != 0))
		lpsched_stat_bump(idx);
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
