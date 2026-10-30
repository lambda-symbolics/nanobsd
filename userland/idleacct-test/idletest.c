/* Test only (docs/power-findings-2026-10-30.org): drive lpsched idle accounting from a kthread on CPU 1; read it with idlerd.  Build in /usr/src/sys/modules/idletest. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kthread.h>
#include <sys/cpu.h>
#include <sys/lpsched.h>

MODULE(MODULE_CLASS_MISC, idletest, NULL);

static lwp_t *it_lwp;
static volatile bool it_stop;

static void
it_thread(void *arg)
{
	while (!it_stop) {
		lpsched_set_idle_depth(1, LPSCHED_IDLE_DEEP);
		kpause("itidle", false, mstohz(300), NULL);
		lpsched_set_idle_depth(1, LPSCHED_IDLE_ACTIVE);
		DELAY(100000);
	}
	kthread_exit(0);
}

static int
idletest_modcmd(modcmd_t cmd, void *arg)
{
	switch (cmd) {
	case MODULE_CMD_INIT:
		lpsched_idle_hooked = 1;
		return kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN,
		    cpu_lookup(1), it_thread, NULL, &it_lwp, "idletest");
	case MODULE_CMD_FINI:
		it_stop = true;
		kthread_join(it_lwp);
		lpsched_idle_hooked = 0;
		return 0;
	default:
		return ENOTTY;
	}
}
