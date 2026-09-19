/*	$NetBSD$	*/

/*
 * lpsched - LISPBSD laptop power scheduler hooks.
 *
 * Policy-free kernel mechanisms driven by the userland governor lpschedd(8)
 * through the machdep.lpsched sysctl tree.  Everything is inert until
 * machdep.lpsched.enabled is set, so a stock kernel is one sysctl away.
 *
 *   - idle-aware packing bias in sched_takecpu()/sched_idle() (kern_runq.c):
 *     place background LWPs on the cheapest CPU to wake instead of scattering
 *     them, and stop idle cores from waking up to steal one stray LWP.
 *   - callout coalescing (kern_timeout.c): round non-precise callouts up to
 *     a slack grid so timers fire in clusters and idle gaps get longer.
 *   - input-activity timestamp from wskbd/wsmouse, exported as idle_ms.
 *
 * The per-CPU idle depth is written by the cidle module's idle loop.
 */

#ifndef _SYS_LPSCHED_H_
#define _SYS_LPSCHED_H_

#include <sys/param.h>
#include <sys/kernel.h>

/* lpsched_idle_depth[] values */
#define	LPSCHED_IDLE_ACTIVE	0	/* running, or depth unknown */
#define	LPSCHED_IDLE_SHALLOW	1	/* short MWAIT, periodic tick running */
#define	LPSCHED_IDLE_DEEP	2	/* tickless MWAIT, expensive to wake */

/* sched_4bsd.c's ESTCPU_SHIFT, mirrored here because it is file-local. */
#define	LPSCHED_ESTCPU_SHIFT	11

extern int	lpsched_enabled;	/* master switch */
extern int	lpsched_pack;		/* 0 off, 1 mild, 2 aggressive */
extern int	lpsched_coalesce_ms;	/* callout slack grid in ms, 0 off */
extern int	lpsched_fgpid;		/* focused process from the WM, 0 none */
extern int	lpsched_estcpu_thresh;	/* estcpu>>SHIFT below this = interactive */
extern volatile int	lpsched_last_input;	/* getticks() at last input */
extern volatile uint8_t	lpsched_idle_depth[MAXCPUS];

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

	if (cpuidx < MAXCPUS)
		lpsched_idle_depth[cpuidx] = depth;
}

#endif /* !_SYS_LPSCHED_H_ */
