/*	$NetBSD$	*/

/*
 * patfix - program the x86 Page Attribute Table on every CPU.
 *
 * NetBSD's pat_init() runs from cpu_attach(), which executes on the boot
 * processor for every CPU it attaches, so the IA32_PAT write only ever
 * lands on CPU 0.  Secondary CPUs keep the power-on PAT, where entry 1
 * (PWT) is Write-Through instead of Write-Combining.  Any mapping the
 * kernel created with PMAP_WRITE_COMBINE (framebuffers, prefetchable
 * BARs) is therefore uncached on CPUs 1..N, roughly 100x slower to write.
 *
 * Loading this module broadcasts the same PAT value pat_init() uses to
 * all CPUs, following the SDM cache-flush protocol for changing PAT.
 * Sysctl machdep.patfix.pat reports the value read back on each CPU.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/cpu.h>
#include <sys/xcall.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

MODULE(MODULE_CLASS_MISC, patfix, NULL);

/* Same encoding as sys/arch/x86/x86/pmap.c pat_init(). */
#define PAT_UC		0x0ULL
#define PAT_WC		0x1ULL
#define PAT_WT		0x4ULL
#define PAT_WP		0x5ULL
#define PAT_WB		0x6ULL
#define PAT_UCMINUS	0x7ULL
#define PATENTRY(n, type)	((type) << ((n) * 8))

static const uint64_t patfix_value =
    PATENTRY(0, PAT_WB) | PATENTRY(1, PAT_WC) |
    PATENTRY(2, PAT_UCMINUS) | PATENTRY(3, PAT_UC) |
    PATENTRY(4, PAT_WB) | PATENTRY(5, PAT_WC) |
    PATENTRY(6, PAT_UCMINUS) | PATENTRY(7, PAT_UC);

static uint64_t		patfix_before[MAXCPUS];
static uint64_t		patfix_after[MAXCPUS];
static struct sysctllog	*patfix_sysctl_log;
static char		patfix_report[MAXCPUS * 24];

static void
patfix_xc(void *arg1, void *arg2)
{
	const u_int ci_idx = cpu_index(curcpu());
	u_long psl;

	psl = x86_read_psl();
	x86_disable_intr();

	patfix_before[ci_idx] = rdmsr(MSR_CR_PAT);
	if (patfix_before[ci_idx] != patfix_value) {
		/* SDM vol. 3A, 11.12.4: flush caches and TLBs around the write. */
		wbinvd();
		wrmsr(MSR_CR_PAT, patfix_value);
		wbinvd();
		tlbflushg();
	}
	patfix_after[ci_idx] = rdmsr(MSR_CR_PAT);

	x86_write_psl(psl);
}

static void
patfix_apply(void)
{
	uint64_t xc;
	CPU_INFO_ITERATOR cii;
	struct cpu_info *ci;
	size_t off = 0;
	int fixed = 0;

	xc = xc_broadcast(0, patfix_xc, NULL, NULL);
	xc_wait(xc);

	for (CPU_INFO_FOREACH(cii, ci)) {
		u_int i = cpu_index(ci);
		if (patfix_before[i] != patfix_after[i])
			fixed++;
		off += snprintf(patfix_report + off, sizeof(patfix_report) - off,
		    "%scpu%u=%#" PRIx64, off ? " " : "", i, patfix_after[i]);
	}
	aprint_normal("patfix: PAT reprogrammed on %d CPU(s); %s\n",
	    fixed, patfix_report);
}

static int
patfix_modcmd(modcmd_t cmd, void *arg)
{
	const struct sysctlnode *node;

	switch (cmd) {
	case MODULE_CMD_INIT:
		patfix_apply();
		sysctl_createv(&patfix_sysctl_log, 0, NULL, &node,
		    CTLFLAG_PERMANENT, CTLTYPE_NODE, "patfix", NULL,
		    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_CREATE, CTL_EOL);
		if (node != NULL)
			sysctl_createv(&patfix_sysctl_log, 0, &node, NULL,
			    CTLFLAG_PERMANENT | CTLFLAG_READONLY, CTLTYPE_STRING,
			    "pat", SYSCTL_DESCR("IA32_PAT as read back per CPU"),
			    NULL, 0, patfix_report, sizeof(patfix_report),
			    CTL_CREATE, CTL_EOL);
		return 0;
	case MODULE_CMD_FINI:
		sysctl_teardown(&patfix_sysctl_log);
		return 0;
	default:
		return ENOTTY;
	}
}
