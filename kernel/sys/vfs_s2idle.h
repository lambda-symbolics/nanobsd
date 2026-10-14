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

/*
 * Suspend every mounted filesystem as one transaction session.  On success
 * the returned entries retain busy mount holds and the VFS suspension lock
 * remains held; the caller must call vfs_resume_all() after device resume.
 * Admission is nonblocking: the base mount iterator reports a mount skipped
 * by try-busy admission, and any such skip aborts the session.  The helper
 * also aborts on updater, fstrans, suspend, or sync errors and unwinds all
 * state before returning no session.
 */
int	vfs_suspend_all(struct vfs_s2idle_mount **, size_t *);
void	vfs_resume_all(struct vfs_s2idle_mount *, size_t);

#endif /* _KERNEL */

#endif /* _SYS_VFS_S2IDLE_H_ */
