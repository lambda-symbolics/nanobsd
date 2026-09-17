/*	$NetBSD$	*/

/*
 * hwp - enable Intel Hardware P-states (HWP, "Speed Shift") on all CPUs
 * and expose the Energy/Performance Preference via sysctl:
 *
 *	machdep.hwp.enabled		(int, RO)  1 once HWP has been enabled
 *	machdep.hwp.epp			(int, RW)  0 = max perf .. 255 = max power save
 *	machdep.hwp.capabilities	(string, RO) decoded IA32_HWP_CAPABILITIES
 *	machdep.hwp.{highest,guaranteed,efficient,lowest} (int, RO)
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

/* Filled in by the first CPU (index 0) to run hwp_xc_enable(). */
static volatile unsigned hwp_ncpus_done;
static uint64_t		hwp_caps_raw;

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

	req  = HWP_REQ_MIN(HWP_CAP_LOWEST(caps));
	req |= HWP_REQ_MAX(HWP_CAP_HIGHEST(caps));
	req |= HWP_REQ_DESIRED(0);		/* autonomous */
	if (hwp_has_epp)
		req |= HWP_REQ_EPP(epp);	/* reserved if !EPP: keep 0 */
	/* activity window (needs HWP_ACTWIN) and package ctl (HWP_PLR): 0 */
	wrmsr(MSR_IA32_HWP_REQUEST, req);

	if (enable) {
		if (cpu_index(curcpu()) == 0)
			hwp_caps_raw = caps;
		atomic_inc_uint(&hwp_ncpus_done);
	}
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
	if (val != hwp_epp) {
		hwp_epp = val;
		hwp_program_all(false);
	}
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
