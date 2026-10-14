/* Exercise the all-mount protocol on a kernel containing vfs_s2idle.c. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/fstrans.h>
#include <sys/vfs_s2idle.h>
#include <prop/proplib.h>

MODULE(MODULE_CLASS_MISC, s2idletest, NULL);

static int
s2idletest_modcmd(modcmd_t cmd, void *arg)
{
	struct vfs_s2idle_mount *mounts;
	size_t nmounts;
	unsigned seconds = 5;
	int error;

	switch (cmd) {
	case MODULE_CMD_INIT:
		if (arg != NULL)
			(void)prop_dictionary_get_uint(arg, "seconds", &seconds);
		if (seconds == 0 || seconds > 60)
			return EINVAL;
		error = vfs_suspend_all(&mounts, &nmounts);
		if (error != 0) {
			printf("s2idletest: barrier error %d\n", error);
			return error;
		}
		for (size_t i = 0; i < nmounts; i++) {
			if (fstrans_getstate(mounts[i].suspend_mp) != FSTRANS_SUSPENDED)
				error = EIO;
			printf("s2idletest: suspended %s (%s)\n",
			    mounts[i].mp->mnt_stat.f_mntonname,
			    mounts[i].mp->mnt_op->vfs_name);
		}
		if (error == 0)
			(void)kpause("s2test", false, seconds * hz, NULL);
		vfs_resume_all(mounts, nmounts);
		printf("s2idletest: resumed %zu mounts, error %d\n", nmounts, error);
		return error;
	case MODULE_CMD_FINI:
		return 0;
	default:
		return ENOTTY;
	}
}
