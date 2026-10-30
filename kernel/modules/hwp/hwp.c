/*	$NetBSD$	*/

/*
 * hwp - enable Intel Hardware P-states (HWP, "Speed Shift") on all CPUs
 * and expose the Energy/Performance Preference via sysctl:
 *
 *	machdep.hwp.enabled		(int, RO)  1 once HWP has been enabled
 *	machdep.hwp.epp			(int, RW)  0 = max perf .. 255 = max power save
 *	machdep.hwp.epp_hw		(int, RO)  EPP every CPU's request register
 *					holds, -1 if they differ from each
 *					other or from what this module writes
 *	machdep.hwp.capabilities	(string, RO) decoded IA32_HWP_CAPABILITIES
 *	machdep.hwp.{highest,guaranteed,efficient,lowest} (int, RO)
 *
 * LISPBSD: and the package power limits (RAPL MSR_PKG_POWER_LIMIT), so a
 * policy can lower the sustained budget on battery and keep the bursts:
 *
 *	machdep.hwp.pl1, machdep.hwp.pl2	(int, RW)  milliwatts
 *	machdep.hwp.pl1_enabled			(int, RO)  PL1_ENABLE bit
 *	machdep.hwp.pl1_window			(int, RO)  ms, PL1 time window
 *	machdep.hwp.pl_locked			(int, RO)  1: firmware locked them
 *	machdep.hwp.pl1_boot, machdep.hwp.pl2_boot (int, RO) as found at load
 *
 * These are the MSR limits only; the firmware may also program the MMIO
 * copy (MCHBAR), and the lower of the two applies.
 *
 * Enabling HWP (IA32_PM_ENABLE bit 0) is one-way until reset; unloading the
 * module only removes the sysctl nodes.
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
#include <machine/specialreg.h>

MODULE(MODULE_CLASS_MISC, hwp, NULL);

/* MSRs (Intel SDM vol. 4); not all are in <machine/specialreg.h>. */
#ifndef MSR_IA32_PM_ENABLE
#define MSR_IA32_PM_ENABLE		0x770
#endif
#ifndef MSR_IA32_HWP_CAPABILITIES
#define MSR_IA32_HWP_CAPABILITIES	0x771
#endif
#ifndef MSR_IA32_HWP_REQUEST_PKG
#define MSR_IA32_HWP_REQUEST_PKG	0x772
#endif
#ifndef MSR_IA32_HWP_REQUEST
#define MSR_IA32_HWP_REQUEST		0x774
#endif

#define PM_ENABLE_HWP			__BIT(0)

#ifndef MSR_RAPL_POWER_UNIT
#define MSR_RAPL_POWER_UNIT		0x606
#endif
#ifndef MSR_PKG_POWER_LIMIT
#define MSR_PKG_POWER_LIMIT		0x610
#endif
#define PL1_POWER			__BITS(14, 0)
#define PL1_ENABLE			__BIT(15)
#define PL1_TIME_Y			__BITS(21, 17)
#define PL1_TIME_Z			__BITS(23, 22)
#define PL2_POWER			__BITS(46, 32)
#define PL2_ENABLE			__BIT(47)
#define PL_LOCK				__BIT(63)

#define HWP_CAP_HIGHEST(v)		((unsigned)((v) >>  0) & 0xff)
#define HWP_CAP_GUARANTEED(v)		((unsigned)((v) >>  8) & 0xff)
#define HWP_CAP_EFFICIENT(v)		((unsigned)((v) >> 16) & 0xff)
#define HWP_CAP_LOWEST(v)		((unsigned)((v) >> 24) & 0xff)

#define HWP_REQ_MIN(v)			((uint64_t)((v) & 0xff) <<  0)
#define HWP_REQ_MAX(v)			((uint64_t)((v) & 0xff) <<  8)
#define HWP_REQ_DESIRED(v)		((uint64_t)((v) & 0xff) << 16)
#define HWP_REQ_EPP(v)			((uint64_t)((v) & 0xff) << 24)
/* bits 41:32 activity window, bit 42 package control: left zero */

#define HWP_EPP_DEFAULT			128	/* balanced */

static kmutex_t		hwp_lock;		/* serialises reprogramming */
static struct sysctllog	*hwp_sysctl_log;

static int		hwp_enabled;		/* sysctl: machdep.hwp.enabled */
static int		hwp_epp = HWP_EPP_DEFAULT; /* sysctl: machdep.hwp.epp */
static int		hwp_cap_highest, hwp_cap_guaranteed;
static int		hwp_cap_efficient, hwp_cap_lowest;
static char		hwp_cap_str[64];

static bool		hwp_has_epp, hwp_has_actwin, hwp_has_plr;

/* RAPL package power limits; hwp_has_rapl when the MSRs read. */
static bool		hwp_has_rapl;
static u_int		hwp_rapl_pu;		/* power unit: 1/2^pu W */
static u_int		hwp_rapl_tu;		/* time unit: 1/2^tu s */
static int		hwp_pl1_boot, hwp_pl2_boot, hwp_pl1_window, hwp_pl_locked;

/* Filled in by the first CPU (index 0) to run hwp_xc_enable(). */
static volatile unsigned hwp_ncpus_done;
static uint64_t		hwp_caps_raw;
static uint64_t		hwp_req_read[MAXCPUS];	/* hwp_xc_read results */

static uint64_t	hwp_request(unsigned);

/*
 * Runs on every CPU via xcall.  arg1 == NULL: first-time enable
 * (write IA32_PM_ENABLE, snapshot capabilities); otherwise just
 * rewrite IA32_HWP_REQUEST.  arg2 points at the EPP value to use.
 */
static void
hwp_xc_program(void *arg1, void *arg2)
{
	const bool enable = (arg1 != NULL);
	const unsigned epp = *(const unsigned *)arg2;
	uint64_t caps, req;

	if (enable) {
		wrmsr(MSR_IA32_PM_ENABLE, PM_ENABLE_HWP);
		/* Ignore the rare case where the write did not stick. */
		if ((rdmsr(MSR_IA32_PM_ENABLE) & PM_ENABLE_HWP) == 0)
			return;
	}

	caps = rdmsr(MSR_IA32_HWP_CAPABILITIES);
	req = hwp_request(epp);
	/* activity window (needs HWP_ACTWIN) and package ctl (HWP_PLR): 0 */
	wrmsr(MSR_IA32_HWP_REQUEST, req);

	if (enable) {
		if (cpu_index(curcpu()) == 0)
			hwp_caps_raw = caps;
		atomic_inc_uint(&hwp_ncpus_done);
	}
}

/*
 * The IA32_HWP_REQUEST this module writes on this CPU, for EPP epp.
 */
static uint64_t
hwp_request(unsigned epp)
{
	uint64_t caps = rdmsr(MSR_IA32_HWP_CAPABILITIES);
	uint64_t req;

	req  = HWP_REQ_MIN(HWP_CAP_LOWEST(caps));
	req |= HWP_REQ_MAX(HWP_CAP_HIGHEST(caps));
	req |= HWP_REQ_DESIRED(0);		/* autonomous */
	if (hwp_has_epp)
		req |= HWP_REQ_EPP(epp);	/* reserved if !EPP: keep 0 */
	return req;
}

/*
 * Runs on every CPU via xcall: record whether its request register is
 * what we would write (low 32 bits: min, max, desired, EPP), and its EPP.
 * Bit 63 of the result flags a mismatch.
 */
static void
hwp_xc_read(void *arg1, void *arg2)
{
	const unsigned epp = *(const unsigned *)arg1;
	uint64_t req = rdmsr(MSR_IA32_HWP_REQUEST);
	uint64_t want = hwp_request(epp);
	u_int i = cpu_index(curcpu());

	if (i < MAXCPUS)
		hwp_req_read[i] = (req & 0xffffffffULL) |
		    ((req & 0xffffffffULL) != (want & 0xffffffffULL) ?
		    __BIT(63) : 0);
}

static void
hwp_program_all(bool enable)
{
	unsigned epp = (unsigned)hwp_epp;
	uint64_t xc;

	KASSERT(mutex_owned(&hwp_lock));
	xc = xc_broadcast(0, hwp_xc_program, enable ? &epp : NULL, &epp);
	xc_wait(xc);
}

static int
hwp_sysctl_epp(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	mutex_enter(&hwp_lock);
	val = hwp_epp;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		goto out;
	if (val < 0 || val > 255) {
		error = EINVAL;
		goto out;
	}
	if (!hwp_has_epp) {
		error = EOPNOTSUPP;
		goto out;
	}
	/*
	 * LISPBSD: always program, even the cached value: a write is how a
	 * caller repairs a request register firmware (or a resume) changed
	 * behind our back, which machdep.hwp.epp_hw reveals.
	 */
	hwp_epp = val;
	hwp_program_all(false);
out:
	mutex_exit(&hwp_lock);
	return error;
}

/*
 * machdep.hwp.epp_hw: read every CPU's IA32_HWP_REQUEST (an xcall: it wakes
 * each CPU, so for occasional verification, not polling).
 */
static int
hwp_sysctl_epp_hw(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	unsigned epp;
	uint64_t xc;
	int val = -2, e;
	u_int i;

	if (!hwp_has_epp)
		return EOPNOTSUPP;
	mutex_enter(&hwp_lock);
	epp = (unsigned)hwp_epp;
	memset(hwp_req_read, 0, sizeof(hwp_req_read));
	xc = xc_broadcast(0, hwp_xc_read, &epp, NULL);
	xc_wait(xc);
	for (CPU_INFO_FOREACH(cii, ci)) {
		i = cpu_index(ci);
		if (i >= MAXCPUS)
			continue;
		e = (int)((hwp_req_read[i] >> 24) & 0xff);
		if ((hwp_req_read[i] & __BIT(63)) != 0 ||
		    (val != -2 && val != e)) {
			val = -1;
			break;
		}
		val = e;
	}
	mutex_exit(&hwp_lock);
	node.sysctl_data = &val;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

/* machdep.hwp.pl1_enabled: whether the firmware or we left PL1 enforced. */
static int
hwp_sysctl_pl1_enabled(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	uint64_t lim;
	int val;

	if (rdmsr_safe(MSR_PKG_POWER_LIMIT, &lim) != 0)
		return EIO;
	val = (lim & PL1_ENABLE) != 0;
	node.sysctl_data = &val;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static int
hwp_raw_to_mw(uint64_t raw)
{
	return (int)((raw * 1000) >> hwp_rapl_pu);
}

static uint64_t
hwp_mw_to_raw(int mw)
{
	return (((uint64_t)mw << hwp_rapl_pu) + 500) / 1000;
}

static void
hwp_rapl_read(void)
{
	uint64_t lim;
	u_int y, z;

	if (rdmsr_safe(MSR_PKG_POWER_LIMIT, &lim) != 0)
		return;
	hwp_pl_locked = (lim & PL_LOCK) != 0;
	y = __SHIFTOUT(lim, PL1_TIME_Y);
	z = __SHIFTOUT(lim, PL1_TIME_Z);
	/* window = 2^y * (1 + z/4) time units */
	hwp_pl1_window = (int)(((1000ULL << y) * (4 + z) / 4) >> hwp_rapl_tu);
}

/* machdep.hwp.pl1 / pl2: arg1 selects the field. */
static int
hwp_sysctl_pl(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	uint64_t lim, field;
	int error, val;

	mutex_enter(&hwp_lock);
	if (rdmsr_safe(MSR_PKG_POWER_LIMIT, &lim) != 0) {
		error = EIO;
		goto out;
	}
	field = rnode->sysctl_data != NULL ? __SHIFTOUT(lim, PL2_POWER) :
	    __SHIFTOUT(lim, PL1_POWER);
	val = hwp_raw_to_mw(field);
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		goto out;
	if (lim & PL_LOCK) {
		error = EPERM;
		goto out;
	}
	/* 3 W .. 64 W: below that the package cannot run, above is a typo. */
	if (val < 3000 || val > 64000) {
		error = EINVAL;
		goto out;
	}
	field = hwp_mw_to_raw(val);
	if (rnode->sysctl_data != NULL) {
		lim &= ~PL2_POWER;
		lim |= __SHIFTIN(field, PL2_POWER) | PL2_ENABLE;
	} else {
		lim &= ~PL1_POWER;
		lim |= __SHIFTIN(field, PL1_POWER) | PL1_ENABLE;
	}
	/* Package scope: any CPU's write applies to the package. */
	wrmsr(MSR_PKG_POWER_LIMIT, lim);
out:
	mutex_exit(&hwp_lock);
	return error;
}

static int
hwp_sysctl_setup(void)
{
	const struct sysctlnode *mnode, *hnode;
	int error;

	error = sysctl_createv(&hwp_sysctl_log, 0, NULL, &mnode,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "machdep", NULL,
	    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_EOL);
	if (error != 0)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &mnode, &hnode,
	    0, CTLTYPE_NODE, "hwp",
	    SYSCTL_DESCR("Intel Hardware P-states (Speed Shift)"),
	    NULL, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "enabled",
	    SYSCTL_DESCR("HWP has been enabled (IA32_PM_ENABLE)"),
	    NULL, 0, &hwp_enabled, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "epp",
	    SYSCTL_DESCR("Energy/Performance Preference "
		"(0 = max performance, 128 = balanced, 255 = max power saving)"),
	    hwp_sysctl_epp, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_STRING, "capabilities",
	    SYSCTL_DESCR("Decoded IA32_HWP_CAPABILITIES"),
	    NULL, 0, hwp_cap_str, sizeof(hwp_cap_str), CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "highest",
	    SYSCTL_DESCR("Highest performance level"),
	    NULL, 0, &hwp_cap_highest, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "guaranteed",
	    SYSCTL_DESCR("Guaranteed performance level"),
	    NULL, 0, &hwp_cap_guaranteed, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "efficient",
	    SYSCTL_DESCR("Most efficient performance level"),
	    NULL, 0, &hwp_cap_efficient, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "lowest",
	    SYSCTL_DESCR("Lowest performance level"),
	    NULL, 0, &hwp_cap_lowest, 0, CTL_CREATE, CTL_EOL);
	if (error != 0 || !hwp_has_rapl)
		return error;

	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "epp_hw",
	    SYSCTL_DESCR("EPP in every CPU's request register, -1 if they"
	    " differ or do not match what this module writes"),
	    hwp_sysctl_epp_hw, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "pl1_enabled",
	    SYSCTL_DESCR("PL1_ENABLE: the PL1 value is enforced"),
	    hwp_sysctl_pl1_enabled, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;

	/* sysctl_data NULL: PL1, non-NULL (any marker): PL2. */
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "pl1",
	    SYSCTL_DESCR("Package long-term power limit (RAPL PL1), mW"),
	    hwp_sysctl_pl, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "pl2",
	    SYSCTL_DESCR("Package short-term power limit (RAPL PL2), mW"),
	    hwp_sysctl_pl, 0, &hwp_pl2_boot, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "pl1_window",
	    SYSCTL_DESCR("PL1 averaging window, ms"),
	    NULL, 0, &hwp_pl1_window, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "pl_locked",
	    SYSCTL_DESCR("The firmware locked the package power limits"),
	    NULL, 0, &hwp_pl_locked, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "pl1_boot",
	    SYSCTL_DESCR("PL1 as found when the module loaded, mW"),
	    NULL, 0, &hwp_pl1_boot, 0, CTL_CREATE, CTL_EOL);
	if (error != 0)
		return error;
	error = sysctl_createv(&hwp_sysctl_log, 0, &hnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_INT, "pl2_boot",
	    SYSCTL_DESCR("PL2 as found when the module loaded, mW"),
	    NULL, 0, &hwp_pl2_boot, 0, CTL_CREATE, CTL_EOL);
	return error;
}

static int
hwp_init(void)
{
	uint32_t regs[4];
	int error;

	/* CPUID.06H must exist and advertise HWP (EAX bit 7). */
	x86_cpuid(0, regs);
	if (regs[0] < 6)
		return ENODEV;
	x86_cpuid(6, regs);
	if ((regs[0] & CPUID_DSPM_HWP) == 0)
		return ENODEV;
	hwp_has_actwin = (regs[0] & CPUID_DSPM_HWP_ACTWIN) != 0;
	hwp_has_epp    = (regs[0] & CPUID_DSPM_HWP_EPP) != 0;
	hwp_has_plr    = (regs[0] & CPUID_DSPM_HWP_PLR) != 0;

	mutex_init(&hwp_lock, MUTEX_DEFAULT, IPL_NONE);

	mutex_enter(&hwp_lock);
	hwp_ncpus_done = 0;
	hwp_program_all(true);
	mutex_exit(&hwp_lock);

	if (hwp_ncpus_done == 0) {
		mutex_destroy(&hwp_lock);
		return ENODEV;
	}

	hwp_cap_highest    = (int)HWP_CAP_HIGHEST(hwp_caps_raw);
	hwp_cap_guaranteed = (int)HWP_CAP_GUARANTEED(hwp_caps_raw);
	hwp_cap_efficient  = (int)HWP_CAP_EFFICIENT(hwp_caps_raw);
	hwp_cap_lowest     = (int)HWP_CAP_LOWEST(hwp_caps_raw);
	snprintf(hwp_cap_str, sizeof(hwp_cap_str),
	    "highest=%d guaranteed=%d efficient=%d lowest=%d",
	    hwp_cap_highest, hwp_cap_guaranteed,
	    hwp_cap_efficient, hwp_cap_lowest);
	hwp_enabled = 1;

	{
		uint64_t unit, lim;

		if (rdmsr_safe(MSR_RAPL_POWER_UNIT, &unit) == 0 &&
		    rdmsr_safe(MSR_PKG_POWER_LIMIT, &lim) == 0) {
			hwp_has_rapl = true;
			hwp_rapl_pu = unit & 0xf;
			hwp_rapl_tu = (unit >> 16) & 0xf;
			hwp_pl1_boot = hwp_raw_to_mw(__SHIFTOUT(lim, PL1_POWER));
			hwp_pl2_boot = hwp_raw_to_mw(__SHIFTOUT(lim, PL2_POWER));
			hwp_rapl_read();
		}
	}

	error = hwp_sysctl_setup();
	if (error != 0) {
		sysctl_teardown(&hwp_sysctl_log);
		mutex_destroy(&hwp_lock);
		return error;
	}

	printf("hwp: HWP enabled on %u CPUs, caps %s, epp=%d%s%s%s\n",
	    hwp_ncpus_done, hwp_cap_str, hwp_epp,
	    hwp_has_epp ? "" : " (EPP unsupported, left 0)",
	    hwp_has_actwin ? " actwin" : "",
	    hwp_has_plr ? " pkg" : "");
	if (hwp_has_rapl)
		printf("hwp: package power limits PL1 %d mW (window %d ms) "
		    "PL2 %d mW%s\n", hwp_pl1_boot, hwp_pl1_window,
		    hwp_pl2_boot, hwp_pl_locked ? ", locked" : "");
	return 0;
}

static int
hwp_fini(void)
{
	sysctl_teardown(&hwp_sysctl_log);
	mutex_destroy(&hwp_lock);
	printf("hwp: module unloaded; HWP remains enabled until reset "
	    "(IA32_PM_ENABLE is one-way), epp=%d\n", hwp_epp);
	return 0;
}

static int
hwp_modcmd(modcmd_t cmd, void *arg)
{
	switch (cmd) {
	case MODULE_CMD_INIT:
		return hwp_init();
	case MODULE_CMD_FINI:
		return hwp_fini();
	default:
		return ENOTTY;
	}
}
