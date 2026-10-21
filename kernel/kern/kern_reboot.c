/*	$NetBSD: kern_reboot.c,v 1.5 2024/03/05 14:15:36 thorpej Exp $	*/

/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_xxx.c	8.3 (Berkeley) 2/14/95
 *	from: NetBSD: kern_xxx.c,v 1.74 2017/10/28 00:37:11 pgoyette Exp
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: kern_reboot.c,v 1.5 2024/03/05 14:15:36 thorpej Exp $");

#include <sys/atomic.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/kernel.h>
#include <sys/kauth.h>
#include <sys/timevar.h>
#include <sys/cpu.h>
#include <sys/sysctl.h>

#include <machine/cpu.h>		/* cpu_reset */
#include <machine/cpu_counter.h>	/* cpu_counter, cpu_frequency */
#include <machine/cpufunc.h>		/* wbinvd */

/*
 * LISPBSD: shutdown breadcrumbs and a no-progress deadline.
 *
 * Some reboots wedge in the kernel's last phase (screen dark, a core
 * spinning) and the software watchdog never fired: its callout is not
 * MPSAFE, so a driver callback stuck in the shutdown walk while holding the
 * kernel lock also holds the watchdog off, and a watchdog panic would wait
 * at the rebooter gate below anyway.  So, independently of both:
 *
 * - lispbsd_shutdown_mark() names each step (an unmount, a detach, a PMF
 *   shutdown callback, a shutdown hook) before it starts, in a record and
 *   on the console, and restarts the deadline;
 * - every CPU checks the deadline in hardclock (cidle keeps the ticks
 *   periodic once shutting_down is set), and the first to see it pass
 *   prints the step that never finished and resets the machine directly.
 *   The message buffer survives that warm reset: read it in the next dmesg.
 *
 * kern.lispbsd_shutdown_timeout: seconds without progress (0: off).
 */
int		lispbsd_shutdown_timeout = 60;
/*
 * TSC value; 0: disarmed.  Not getticks(): that only advances on the
 * primary CPU's tick, which is the one stuck if the hang runs at high IPL
 * there.  The TSC is invariant and synchronised across cores here.
 */
volatile uint64_t lispbsd_shutdown_deadline;
static const char *volatile lispbsd_shutdown_stage = "";
static char	lispbsd_shutdown_what[64];
static volatile u_int lispbsd_shutdown_fired;

void	lispbsd_shutdown_mark(const char *, const char *);
void	lispbsd_shutdown_disarm(void);
void	lispbsd_shutdown_tick(void);

void
lispbsd_shutdown_mark(const char *stage, const char *what)
{

	if (!shutting_down || lispbsd_shutdown_timeout <= 0)
		return;
	lispbsd_shutdown_stage = stage;
	strlcpy(lispbsd_shutdown_what, what != NULL ? what : "",
	    sizeof(lispbsd_shutdown_what));
	lispbsd_shutdown_deadline = cpu_counter() +
	    (uint64_t)lispbsd_shutdown_timeout * cpu_frequency(curcpu());
	printf("shutdown: %s %s\n", stage, lispbsd_shutdown_what);
	/*
	 * Into RAM now: a reset discards dirty cache lines, so the newest
	 * breadcrumbs (the ones that matter) would be lost, and wbinvd on
	 * the CPU that notices the deadline does not reach this CPU's cache.
	 */
	wbinvd();
}

void
lispbsd_shutdown_disarm(void)
{

	lispbsd_shutdown_deadline = 0;
}

/* hardclock, any CPU: the step named last has not finished in time. */
void
lispbsd_shutdown_tick(void)
{

	if (atomic_swap_uint(&lispbsd_shutdown_fired, 1) != 0)
		return;
	printf("shutdown watchdog: no progress for %d s, stuck in: %s %s "
	    "(noticed on cpu%u); resetting\n", lispbsd_shutdown_timeout,
	    lispbsd_shutdown_stage, lispbsd_shutdown_what,
	    cpu_index(curcpu()));
	delay(500000);
	/*
	 * Write the message buffer back to RAM: a reset discards dirty cache
	 * lines, and the newest lines (these) were lost without this.
	 */
	wbinvd();
	cpu_reset();
}

/*
 * kern.lispbsd_shutdown_test_spin: a test of the above.  Writing N > 0
 * adds a shutdown hook that spins at splhigh for N seconds, standing in for
 * a stuck driver; with the timeout below N the next reboot must end in the
 * watchdog's reset, and the next boot's dmesg must name the hook.
 */
static int	lispbsd_shutdown_test_spin;

static void
lispbsd_shutdown_test_hook(void *arg)
{
	int i;

	printf("shutdown: test hook spinning for %d s\n",
	    lispbsd_shutdown_test_spin);
	for (i = 0; i < lispbsd_shutdown_test_spin * 1000; i++)
		delay(1000);
}

static int
sysctl_lispbsd_shutdown_test_spin(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	static bool hooked;
	int error, val = lispbsd_shutdown_test_spin;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (val < 0 || val > 600)
		return EINVAL;
	lispbsd_shutdown_test_spin = val;
	if (val > 0 && !hooked) {
		hooked = true;
		(void)shutdownhook_establish(lispbsd_shutdown_test_hook, NULL);
	}
	return 0;
}

SYSCTL_SETUP(sysctl_kern_lispbsd_shutdown_setup, "kern.lispbsd_shutdown_timeout")
{

	sysctl_createv(clog, 0, NULL, NULL, CTLFLAG_PERMANENT |
	    CTLFLAG_READWRITE, CTLTYPE_INT, "lispbsd_shutdown_test_spin",
	    SYSCTL_DESCR("Test: a shutdown hook spins this many seconds"),
	    sysctl_lispbsd_shutdown_test_spin, 0, NULL, 0, CTL_KERN,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, NULL, NULL, CTLFLAG_PERMANENT |
	    CTLFLAG_READWRITE, CTLTYPE_INT, "lispbsd_shutdown_timeout",
	    SYSCTL_DESCR("Reset a shutdown that makes no progress for this "
		"many seconds, naming the step it is stuck in (0: off)"),
	    NULL, 0, &lispbsd_shutdown_timeout, 0, CTL_KERN, CTL_CREATE,
	    CTL_EOL);
}

/*
 * Reboot / shutdown the system.
 */
void
kern_reboot(int howto, char *bootstr)
{
	static lwp_t *rebooter;
	lwp_t *l;

	/*
	 * If already rebooting then just hang out.  Allow reentry for the
	 * benfit of ddb, even if questionable.  It would be better to call
	 * exit1(), but this is called from all sorts of places.
	 */
	l = atomic_cas_ptr(&rebooter, NULL, curlwp);
	while (l != NULL && l != curlwp) {
		(void)kpause("reboot", true, 0, NULL);
	}
	shutting_down = 1;
	if (panicstr == NULL)
		lispbsd_shutdown_mark("kern_reboot", curproc->p_comm);

	/* Just cut to the chase if cold. */
	if (cold) {
		goto do_cpu_reboot;
	}

	if ((boothowto & RB_NOSYNC) == 0 && panicstr == NULL) {
		/*
		 * If we've been adjusting the clock, the todr
		 * will be out of synch; adjust it now.
		 */
		if (time_adjusted != 0) {
			time_adjusted = 0;
			resettodr();
		}
	}

	/*
	 * XXX We should re-factor out all of the common stuff
	 * that each and every cpu_reboot() does and put it here.
	 */

 do_cpu_reboot:
	cpu_reboot(howto, bootstr);
}

/* ARGSUSED */
int
sys_reboot(struct lwp *l, const struct sys_reboot_args *uap, register_t *retval)
{
	/* {
		syscallarg(int) opt;
		syscallarg(char *) bootstr;
	} */
	int error;
	char *bootstr, bs[128];

	if ((error = kauth_authorize_system(l->l_cred, KAUTH_SYSTEM_REBOOT,
	    0, NULL, NULL, NULL)) != 0)
		return (error);

	/*
	 * Only use the boot string if RB_STRING is set.
	 */
	if ((SCARG(uap, opt) & RB_STRING) &&
	    (error = copyinstr(SCARG(uap, bootstr), bs, sizeof(bs), 0)) == 0)
		bootstr = bs;
	else
		bootstr = NULL;
	/*
	 * Not all ports use the bootstr currently.
	 */
	kern_reboot(SCARG(uap, opt), bootstr);
	return (0);
}
