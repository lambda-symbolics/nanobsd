/*	$NetBSD$	*/

/*
 * cidle - replace the x86 idle routine with MONITOR/MWAIT using a tunable
 * C-state hint, so the CPU can reach C6/C8/C10 instead of the C1 that
 * NetBSD 11's acpicpu(4) always picks (its idle-time measurement is
 * compiled out, so acpicpu_cstate_latency() never returns anything deeper).
 *
 *	machdep.cidle.hint	(int, RW)  MWAIT EAX hint: 0x00 C1, 0x01 C1E,
 *				0x10 C3, 0x20 C6, 0x30 C7, 0x40 C8, 0x50 C9, 0x60 C10
 *	machdep.cidle.active	(int, RW)  1 = cidle idle installed, 0 = previous
 *	machdep.cidle.previous	(string, RO) name of the idle routine replaced
 *
 * Tickless idle (dynamic tick): when enabled, an idle CPU stops the 100 Hz
 * periodic LAPIC tick and sleeps in a single MWAIT until the next callout is
 * due (bounded by machdep.cidle.maxskip), then restores the tick and replays
 * the elapsed hardclock() ticks.  This lets the CPU package dwell in deep C
 * states (PC10) for up to ~1s at a time instead of being woken every 10ms,
 * which is what unlocks true S0ix-class residency both at runtime and during
 * s2idle suspend.
 *
 *	machdep.cidle.tickless	(int, RW)  1 = stop the periodic tick when idle
 *	machdep.cidle.maxskip	(int, RW)  max hardclock ticks to skip (cap)
 *	machdep.cidle.minskip	(int, RW)  don't go tickless for fewer than this
 *	machdep.cidle.tlstats	(string, RO) tickless counters
 *
 * Unloading the module restores the previous idle routine.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/mutex.h>
#include <sys/cpu.h>
#include <sys/xcall.h>
#include <sys/atomic.h>

#include <machine/cpufunc.h>
#include <machine/cpu.h>

MODULE(MODULE_CLASS_MISC, cidle, NULL);

void x86_cpu_idle_set(void (*)(void), const char *, bool);
void x86_cpu_idle_get(void (**)(void), char *, size_t);

/* Tickless primitives exported by the kernel (lapic.c, kern_timeout.c,
 * kern_heartbeat.c, kern_clock.c). */
struct clockframe;
void		lapic_oneshot(uint32_t);
unsigned	lapic_oneshot_done(uint32_t, int *);
int		callout_next_ticks(void);
void		heartbeat_suspend(void);
void		heartbeat_resume(void);
void		hardclock(struct clockframe *);
extern uint32_t	lapic_tval;

static kmutex_t		cidle_lock;
static struct sysctllog	*cidle_sysctl_log;
static volatile int	cidle_hint = 0x60;	/* default: deepest MWAIT hint (C10); core still gates at C7 */
static int		cidle_active;
static volatile int	cidle_tickless = 0;	/* default off: enable via sysctl */
static volatile int	cidle_maxskip = 100;	/* cap ~1s at hz=100 (well under 15s heartbeat) */
static volatile int	cidle_minskip = 4;	/* skip only if >=4 ticks (40ms) to gain */
static volatile int	cidle_forceskip = 0;	/* >0: force this skip (ignore callouts), still capped by maxskip */
static void		(*cidle_prev_func)(void);
static char		cidle_prev_text[16];
static uint32_t		cidle_cpuid5_edx;
static uint32_t		cidle_cpuid5_ecx;	/* bit1 = IBE (irq break) */
#define CPUID5_ECX_IBE	(1u << 1)
static char		cidle_resid[512];
static char		cidle_tlstats[256];

/* Tickless statistics (racy across CPUs; for observation only). */
static volatile uint64_t cidle_tl_entries;	/* times we stopped the tick */
static volatile uint64_t cidle_tl_ticks;	/* total hardclock ticks skipped */
static volatile uint64_t cidle_tl_early;	/* woke before the one-shot fired */
static volatile uint64_t cidle_tl_full;		/* one-shot expired (deep sleep) */

/* Synthetic clockframe for replayed hardclock() ticks: all-zero => kernel
 * mode (CLKF_USERMODE false), so catch-up is accounted as idle/system. */
static uint8_t		cidle_cf[256] __aligned(16);

/* Intel C-state residency MSRs and the package C-state config register. */
#define MSR_CORE_C3_RES	0x3fc
#define MSR_CORE_C6_RES	0x3fd
#define MSR_CORE_C7_RES	0x3fe
#define MSR_PKG_C2_RES	0x60d
#define MSR_PKG_C3_RES	0x3f8
#define MSR_PKG_C6_RES	0x3f9
#define MSR_PKG_C7_RES	0x3fa
#define MSR_PKG_C8_RES	0x630
#define MSR_PKG_C9_RES	0x631
#define MSR_PKG_C10_RES	0x632
#define MSR_PKG_CST_CFG	0x0e2
#define MSR_SMI_COUNT	0x34

static void
cidle_xc_resid(void *arg1, void *arg2)
{
	/* runs on CPU 0 only */
	snprintf(cidle_resid, sizeof(cidle_resid),
	    "tsc=%" PRIu64 " c3=%" PRIu64 " c6=%" PRIu64 " c7=%" PRIu64
	    " pc2=%" PRIu64 " pc3=%" PRIu64 " pc6=%" PRIu64 " pc7=%" PRIu64
	    " pc8=%" PRIu64 " pc9=%" PRIu64 " pc10=%" PRIu64 " cstcfg=%#" PRIx64
	    " smi=%" PRIu64,
	    rdtsc(), rdmsr(MSR_CORE_C3_RES), rdmsr(MSR_CORE_C6_RES),
	    rdmsr(MSR_CORE_C7_RES), rdmsr(MSR_PKG_C2_RES), rdmsr(MSR_PKG_C3_RES),
	    rdmsr(MSR_PKG_C6_RES), rdmsr(MSR_PKG_C7_RES), rdmsr(MSR_PKG_C8_RES),
	    rdmsr(MSR_PKG_C9_RES), rdmsr(MSR_PKG_C10_RES), rdmsr(MSR_PKG_CST_CFG),
	    rdmsr(MSR_SMI_COUNT));
}

static int
cidle_sysctl_resid(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	uint64_t xc;

	xc = xc_unicast(0, cidle_xc_resid, NULL, NULL, cpu_lookup(0));
	xc_wait(xc);
	node.sysctl_data = cidle_resid;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int
cidle_sysctl_tlstats(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;

	snprintf(cidle_tlstats, sizeof(cidle_tlstats),
	    "entries=%" PRIu64 " ticks_skipped=%" PRIu64 " deep=%" PRIu64
	    " woke_early=%" PRIu64,
	    cidle_tl_entries, cidle_tl_ticks, cidle_tl_full, cidle_tl_early);
	node.sysctl_data = cidle_tlstats;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

/*
 * Tickless idle: stop the periodic tick and sleep until the next callout is
 * due (or maxskip ticks), then restore the tick and replay elapsed ticks.
 * Runs on every idle CPU independently.  Returns with interrupts enabled.
 */
static void
cidle_idle_tickless(struct cpu_info *ci)
{
	int skip, pending, s;
	unsigned elapsed, manual, n;

	x86_disable_intr();
	x86_monitor(&ci->ci_want_resched, 0, 0);
	if (__predict_false(ci->ci_want_resched != 0)) {
		x86_enable_intr();
		return;
	}

	skip = callout_next_ticks();
	if (cidle_forceskip > 0)
		skip = cidle_forceskip;	/* force sustained deep idle; callouts fire late but are replayed */
	if (skip > cidle_maxskip)
		skip = cidle_maxskip;	/* heartbeat safety: maxskip <= 1400 (< 15s) */

	if (skip < cidle_minskip) {
		/*
		 * Not worth stopping the tick: sleep one mwait; the periodic
		 * timer wakes us within a tick and delivers hardclock on sti.
		 * ecx=1 => interrupts break MWAIT even though IF=0.
		 */
		x86_mwait((uint32_t)cidle_hint, 1);
		x86_enable_intr();
		return;
	}

	/* Stop the tick and go deep. */
	heartbeat_suspend();
	lapic_oneshot((uint32_t)skip);
	x86_mwait((uint32_t)cidle_hint, 1);

	/* Woke (IF still 0, no ISR ran).  Reconcile and restore the tick. */
	elapsed = lapic_oneshot_done((uint32_t)skip, &pending);

	manual = (elapsed >= (unsigned)pending) ? elapsed - (unsigned)pending : 0;
	if (manual > 0) {
		s = splclock();
		for (n = 0; n < manual; n++)
			hardclock((struct clockframe *)cidle_cf);
		splx(s);
	}

	heartbeat_resume();

	atomic_inc_64(&cidle_tl_entries);
	atomic_add_64(&cidle_tl_ticks, elapsed);
	if (pending)
		atomic_inc_64(&cidle_tl_full);
	else
		atomic_inc_64(&cidle_tl_early);

	/* Deliver the pending timer IRR (if any) and any device IRQ. */
	x86_enable_intr();
}

static void
cidle_idle(void)
{
	struct cpu_info *ci = curcpu();

	KASSERT(ci->ci_ilevel == IPL_NONE);

	if (__predict_false(cidle_tickless)) {
		cidle_idle_tickless(ci);
		return;
	}

	x86_monitor(&ci->ci_want_resched, 0, 0);
	if (__predict_false(ci->ci_want_resched != 0))
		return;
	x86_mwait((uint32_t)cidle_hint, 0);
}

static void
cidle_xc_nop(void *a, void *b)
{
	/* Kicks every CPU out of its idle routine. */
}

static void
cidle_install(bool on)
{
	uint64_t xc;

	KASSERT(mutex_owned(&cidle_lock));
	if (on == (cidle_active != 0))
		return;
	if (on)
		x86_cpu_idle_set(cidle_idle, "cidle", false);
	else
		x86_cpu_idle_set(cidle_prev_func, cidle_prev_text, false);
	cidle_active = on;
	xc = xc_broadcast(0, cidle_xc_nop, NULL, NULL);
	xc_wait(xc);
}

static int
cidle_sysctl_hint(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	mutex_enter(&cidle_lock);
	val = cidle_hint;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val < 0 || val > 0xff || (val & 0x0f) > 0x0f)
			error = EINVAL;
		else
			cidle_hint = val;
	}
	mutex_exit(&cidle_lock);
	return error;
}

static int
cidle_sysctl_active(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	mutex_enter(&cidle_lock);
	val = cidle_active;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val != 0 && val != 1)
			error = EINVAL;
		else
			cidle_install(val != 0);
	}
	mutex_exit(&cidle_lock);
	return error;
}

static int
cidle_sysctl_tickless(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = cidle_tickless;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val != 0 && val != 1)
			error = EINVAL;
		else if (val == 1 && (cidle_cpuid5_ecx & CPUID5_ECX_IBE) == 0)
			error = ENODEV;	/* MWAIT can't wake on masked IRQ */
		else
			cidle_tickless = val;
	}
	return error;
}

static int
cidle_sysctl_maxskip(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = cidle_maxskip;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val < 1 || val > 1400)	/* stay well under 15s heartbeat */
			error = EINVAL;
		else
			cidle_maxskip = val;
	}
	return error;
}

static int
cidle_sysctl_minskip(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = cidle_minskip;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val < 1 || val > 1000)
			error = EINVAL;
		else
			cidle_minskip = val;
	}
	return error;
}

static int
cidle_sysctl_forceskip(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = cidle_forceskip;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error == 0 && newp != NULL) {
		if (val < 0 || val > 1400)	/* 0 = off; capped like maxskip */
			error = EINVAL;
		else
			cidle_forceskip = val;
	}
	return error;
}

static void
cidle_sysctl_setup(void)
{
	const struct sysctlnode *node = NULL;

	sysctl_createv(&cidle_sysctl_log, 0, NULL, &node, CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "cidle", NULL, NULL, 0, NULL, 0,
	    CTL_MACHDEP, CTL_CREATE, CTL_EOL);
	if (node == NULL)
		return;
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "hint",
	    SYSCTL_DESCR("MWAIT C-state hint used when idle"),
	    cidle_sysctl_hint, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "active",
	    SYSCTL_DESCR("1 if the cidle idle routine is installed"),
	    cidle_sysctl_active, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "tickless",
	    SYSCTL_DESCR("1 = stop the periodic tick when idle (dynamic tick)"),
	    cidle_sysctl_tickless, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "maxskip",
	    SYSCTL_DESCR("max hardclock ticks the idle path may skip"),
	    cidle_sysctl_maxskip, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "minskip",
	    SYSCTL_DESCR("minimum ticks worth stopping the tick for"),
	    cidle_sysctl_minskip, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "forceskip",
	    SYSCTL_DESCR("0=off; >0 forces this idle skip (ignore callouts) for sustained C10"),
	    cidle_sysctl_forceskip, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "tlstats",
	    SYSCTL_DESCR("tickless idle counters"),
	    cidle_sysctl_tlstats, 0, NULL, sizeof(cidle_tlstats),
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "residency",
	    SYSCTL_DESCR("CPU0 core/package C-state residency MSRs"),
	    cidle_sysctl_resid, 0, NULL, sizeof(cidle_resid),
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(&cidle_sysctl_log, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING, "previous",
	    SYSCTL_DESCR("idle routine that cidle replaced"),
	    NULL, 0, cidle_prev_text, sizeof(cidle_prev_text),
	    CTL_CREATE, CTL_EOL);
}

static int
cidle_modcmd(modcmd_t cmd, void *arg)
{
	uint32_t regs[4];

	switch (cmd) {
	case MODULE_CMD_INIT:
		if ((curcpu()->ci_feat_val[1] & CPUID2_MONITOR) == 0) {
			aprint_error("cidle: CPU lacks MONITOR/MWAIT\n");
			return ENODEV;
		}
		x86_cpuid(5, regs);
		cidle_cpuid5_edx = regs[3];
		cidle_cpuid5_ecx = regs[2];
		mutex_init(&cidle_lock, MUTEX_DEFAULT, IPL_NONE);
		x86_cpu_idle_get(&cidle_prev_func, cidle_prev_text,
		    sizeof(cidle_prev_text));
		mutex_enter(&cidle_lock);
		cidle_install(true);
		mutex_exit(&cidle_lock);
		cidle_sysctl_setup();
		aprint_normal("cidle: installed (was \"%s\"); hint=%#x; "
		    "CPUID.5 EDX (mwait sub-states per C-state)=%#x\n",
		    cidle_prev_text, cidle_hint, cidle_cpuid5_edx);
		return 0;
	case MODULE_CMD_FINI:
		mutex_enter(&cidle_lock);
		cidle_tickless = 0;
		cidle_install(false);
		mutex_exit(&cidle_lock);
		sysctl_teardown(&cidle_sysctl_log);
		/* let any CPU still returning from cidle_idle() get out */
		DELAY(10000);
		mutex_destroy(&cidle_lock);
		return 0;
	default:
		return ENOTTY;
	}
}
