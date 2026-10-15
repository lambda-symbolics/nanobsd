/*	$NetBSD$ */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fstrans.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/vfs_s2idle.h>

/*
 * Mount administration and all-mount suspension share an admission gate.
 * Count complete operations, including nested ones, rather than holding a
 * lock across filesystem callbacks. A callback that tries to mount while
 * the barrier is held gets EBUSY instead of waiting on its own suspension.
 */
static kmutex_t vfs_s2idle_lock;
static size_t vfs_s2idle_mountops;
static struct lwp *vfs_s2idle_owner;
/* Published once during VFS initialization, before runtime mount activity. */
static bool vfs_s2idle_ready;

void
vfs_s2idle_init(void)
{

	KASSERT(!vfs_s2idle_ready);
	mutex_init(&vfs_s2idle_lock, MUTEX_DEFAULT, IPL_NONE);
	vfs_s2idle_ready = true;
}

int
vfs_s2idle_mount_enter(void)
{
	int error = 0;

	if (!vfs_s2idle_ready)
		return ENXIO;
	mutex_enter(&vfs_s2idle_lock);
	if (vfs_s2idle_owner != NULL || vfs_s2idle_mountops == SIZE_MAX)
		error = EBUSY;
	else
		vfs_s2idle_mountops++;
	mutex_exit(&vfs_s2idle_lock);
	return error;
}

void
vfs_s2idle_mount_exit(void)
{

	mutex_enter(&vfs_s2idle_lock);
	KASSERT(vfs_s2idle_owner == NULL);
	KASSERT(vfs_s2idle_mountops != 0);
	vfs_s2idle_mountops--;
	mutex_exit(&vfs_s2idle_lock);
}

static int
vfs_s2idle_barrier_enter(void)
{
	int error = 0;

	/* Reject an assembled kernel that still has only the old iterator patch. */
	if (!vfs_s2idle_ready)
		return ENXIO;
	mutex_enter(&vfs_s2idle_lock);
	if (vfs_s2idle_owner != NULL || vfs_s2idle_mountops != 0)
		error = EBUSY;
	else
		vfs_s2idle_owner = curlwp;
	mutex_exit(&vfs_s2idle_lock);
	return error;
}

static void
vfs_s2idle_barrier_exit(void)
{

	mutex_enter(&vfs_s2idle_lock);
	KASSERT(vfs_s2idle_owner == curlwp);
	KASSERT(vfs_s2idle_mountops == 0);
	vfs_s2idle_owner = NULL;
	mutex_exit(&vfs_s2idle_lock);
}

static struct mount *
vfs_s2idle_canonical_mount(struct mount *mp)
{
	struct mount *slow, *fast;

	/* A corrupt lower-mount cycle must not hang suspend preparation. */
	slow = fast = mp;
	while (fast != NULL && fast->mnt_lower != NULL) {
		slow = slow->mnt_lower;
		fast = fast->mnt_lower;
		if (fast != NULL)
			fast = fast->mnt_lower;
		if (slow == fast)
			return NULL;
	}
	while (mp->mnt_lower != NULL)
		mp = mp->mnt_lower;
	return mp;
}

static bool
vfs_s2idle_has_mount(struct vfs_s2idle_mount *mounts, size_t nmounts,
    struct mount *mp)
{

	for (size_t i = 0; i < nmounts; i++) {
		if (mounts[i].mp == mp)
			return true;
	}
	return false;
}

static bool
vfs_s2idle_same_updater(struct vfs_s2idle_mount *mounts, size_t nmounts,
    size_t index)
{

	for (size_t i = 0; i < nmounts; i++) {
		if (i != index && mounts[i].updating_held &&
		    mounts[i].mp->mnt_updating == mounts[index].mp->mnt_updating)
			return true;
	}
	return false;
}

static void
vfs_s2idle_unlock_updates(struct vfs_s2idle_mount *mounts, size_t nmounts)
{

	for (size_t i = 0; i < nmounts; i++) {
		if (mounts[i].updating_held) {
			mutex_exit(mounts[i].mp->mnt_updating);
			mounts[i].updating_held = 0;
		}
	}
}

static void
vfs_s2idle_resume_suspended(struct vfs_s2idle_mount *mounts, size_t nmounts)
{

	/* Resume backing filesystems before the mounts layered on top. */
	for (size_t i = 0; i < nmounts; i++) {
		if (!mounts[i].suspended || mounts[i].global_lock)
			continue;
		(void)VFS_SUSPENDCTL(mounts[i].suspend_mp, SUSPEND_RESUME);
		mounts[i].suspended = 0;
	}
	for (size_t i = 0; i < nmounts; i++) {
		if (!mounts[i].suspended || !mounts[i].global_lock)
			continue;
		vfs_resume(mounts[i].suspend_mp);
		mounts[i].suspended = 0;
		mounts[i].global_lock = 0;
	}
}

/*
 * Suspend the filesystem set as one vfs_suspend() session.
 *
 * Admission rejects active mount operations and excludes new ones until
 * resume. The extended mount iterator reports mounts skipped by try-busy;
 * any skip aborts rather than producing a partial filesystem set. Each saved
 * mount takes an additional busy hold and an updater try-lock before any
 * filesystem is suspended. Transaction draining and synchronous writes may
 * sleep; only admission is non-waiting.
 *
 * Suspend and sync each canonical mount in reverse creation order before
 * stopping its backing filesystem. Aliases share their canonical mount's
 * position. Dependencies must follow that order; mnt_lower cycles are errors.
 */
int
vfs_suspend_all(struct vfs_s2idle_mount **mpp, size_t *nmpp)
{
	mount_iterator_t *iter;
	struct vfs_s2idle_mount *mounts = NULL;
	struct mount *mp;
	size_t nmounts = 0, capacity = 0;
	int error = 0;
	int skipped;
	int asyncflag;
	bool global_held = false;

	KASSERT(mpp != NULL);
	KASSERT(nmpp != NULL);
	*mpp = NULL;
	*nmpp = 0;

	if ((error = vfs_s2idle_barrier_enter()) != 0)
		return error;

	mountlist_iterator_init(&iter);
	for (;;) {
		skipped = 0;
		mp = mountlist_iterator_trynext_ex(iter, &skipped);
		if (skipped) {
			error = EBUSY;
			break;
		}
		if (mp == NULL)
			break;
		if (nmounts == capacity) {
			size_t newcapacity = capacity != 0 ? capacity * 2 : 16;
			struct vfs_s2idle_mount *newmounts;

			if (newcapacity < capacity ||
			    newcapacity > SIZE_MAX / sizeof(*newmounts)) {
				error = ENOMEM;
				break;
			}
			newmounts = kmem_alloc(newcapacity * sizeof(*newmounts),
			    KM_SLEEP);
			if (mounts != NULL) {
				memcpy(newmounts, mounts,
				    nmounts * sizeof(*newmounts));
				kmem_free(mounts, capacity * sizeof(*mounts));
			}
			mounts = newmounts;
			capacity = newcapacity;
		}

		if ((error = vfs_trybusy(mp)) != 0)
			break;
		mounts[nmounts].mp = mp;
		mounts[nmounts].suspend_mp = NULL;
		mounts[nmounts].suspended = 0;
		mounts[nmounts].global_lock = 0;
		mounts[nmounts].updating_held = 0;
		nmounts++;
	}
	mountlist_iterator_destroy(iter);

	if (error != 0)
		goto fail;
	if (nmounts == 0) {
		if (mounts != NULL)
			kmem_free(mounts, capacity * sizeof(*mounts));
		return 0;
	}
	/* Allocate the final session before stopping filesystem transactions. */
	if (capacity != nmounts) {
		struct vfs_s2idle_mount *exact;

		exact = kmem_alloc(nmounts * sizeof(*exact), KM_SLEEP);
		memcpy(exact, mounts, nmounts * sizeof(*exact));
		kmem_free(mounts, capacity * sizeof(*mounts));
		mounts = exact;
		capacity = nmounts;
	}

	for (size_t i = 0; i < nmounts; i++) {
		mounts[i].suspend_mp = vfs_s2idle_canonical_mount(mounts[i].mp);
		if (mounts[i].suspend_mp == NULL) {
			error = ELOOP;
			goto fail;
		}
		if (!vfs_s2idle_has_mount(mounts, nmounts, mounts[i].suspend_mp)) {
			error = EBUSY;
			goto fail;
		}
	}

	/* Admit all update locks before changing any fstrans state. */
	for (size_t i = nmounts; i-- > 0;) {
		if (vfs_s2idle_same_updater(mounts, nmounts, i))
			continue;
		if (mounts[i].mp->mnt_updating == NULL ||
		    !mutex_tryenter(mounts[i].mp->mnt_updating)) {
			error = EBUSY;
			goto fail;
		}
		mounts[i].updating_held = 1;
	}

	/*
	 * Use each canonical mount's creation order, not an alias's later
	 * position. Flush it before suspending its backing filesystem: an
	 * FFS-on-vnd sync can need another thread in that backing filesystem.
	 */
	for (size_t i = nmounts; i-- > 0;) {
		mp = mounts[i].mp;
		if (mp != mounts[i].suspend_mp)
			continue;
		if (!global_held)
			error = vfs_suspend(mp, 1);
		else
			error = VFS_SUSPENDCTL(mp, SUSPEND_SUSPEND);
		if (error != 0)
			goto rollback;
		mounts[i].suspended = 1;
		if (!global_held) {
			mounts[i].global_lock = 1;
			global_held = true;
		}
		/* Sync this mount and all aliases while we own its transaction. */
		for (size_t j = nmounts; j-- > 0;) {
			if (mounts[j].suspend_mp != mounts[i].mp)
				continue;
			mp = mounts[j].mp;
			asyncflag = mp->mnt_flag & MNT_ASYNC;
			mp->mnt_flag &= ~MNT_ASYNC;
			error = VFS_SYNC(mp, MNT_WAIT, curlwp->l_cred);
			mp->mnt_flag |= asyncflag;
			if (error != 0)
				goto rollback;
		}
	}

	*mpp = mounts;
	*nmpp = nmounts;
	return 0;

rollback:
	vfs_s2idle_resume_suspended(mounts, nmounts);
fail:
	vfs_s2idle_unlock_updates(mounts, nmounts);
	for (size_t i = 0; i < nmounts; i++)
		vfs_unbusy(mounts[i].mp);
	if (mounts != NULL)
		kmem_free(mounts, capacity * sizeof(*mounts));
	vfs_s2idle_barrier_exit();
	return error;
}

void
vfs_resume_all(struct vfs_s2idle_mount *mounts, size_t nmounts)
{

	KASSERT(mounts != NULL || nmounts == 0);
	if (nmounts == 0) {
		vfs_s2idle_barrier_exit();
		return;
	}

	vfs_s2idle_resume_suspended(mounts, nmounts);
	vfs_s2idle_unlock_updates(mounts, nmounts);
	for (size_t i = 0; i < nmounts; i++)
		vfs_unbusy(mounts[i].mp);
	kmem_free(mounts, nmounts * sizeof(*mounts));
	vfs_s2idle_barrier_exit();
}
