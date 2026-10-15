/*	$NetBSD$ */

#ifndef _SYS_VFS_S2IDLE_H_
#define	_SYS_VFS_S2IDLE_H_

#include <sys/types.h>

#ifdef _KERNEL

struct mount;

struct vfs_s2idle_mount {
	struct mount *mp;
	/* Canonical mount whose fstrans state this entry suspended. */
	struct mount *suspend_mp;
	/* Set only on the entry that performed a suspendctl operation. */
	int suspended;
	/* This entry owns the non-recursive vfs_suspend lock. */
	int global_lock;
	/* This entry owns mnt_updating until resume or rollback. */
	int updating_held;
};

/* Initialize once from vfs_mount_sysinit(). */
void	vfs_s2idle_init(void);

/*
 * Cover complete mount, update, unmount and mountroot operations. Entry is
 * non-waiting and returns EBUSY while an all-mount barrier is held. Every
 * successful entry needs an exit, including nested operations and failures.
 */
int	vfs_s2idle_mount_enter(void);
void	vfs_s2idle_mount_exit(void);

/*
 * Suspend every mounted filesystem as one transaction session. The helper
 * first excludes mount administration, then retains busy mount holds, updater
 * locks and the VFS suspension lock. Admission returns EBUSY if a mount
 * operation is active, or the iterator skips a mount. Filesystem transaction
 * draining and synchronous writes may sleep.
 *
 * On every success, even an empty mount set, the same LWP must call
 * vfs_resume_all() after device resume. On error all acquired state is unwound;
 * do not call vfs_resume_all() for a failed or skipped attempt.
 */
int	vfs_suspend_all(struct vfs_s2idle_mount **, size_t *);
void	vfs_resume_all(struct vfs_s2idle_mount *, size_t);

#endif /* _KERNEL */

#endif /* _SYS_VFS_S2IDLE_H_ */
