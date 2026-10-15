#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/vfs_s2idle.h>

#define MAX_MOUNTS 64
#define MAX_EVENTS 256

struct fake_mutex {
	int held;
	int id;
	/* Used only by the admission gate, not the mock updater locks. */
	pthread_mutex_t native;
};

struct mount {
	struct mount *mnt_lower;
	struct mount *backing;
	int mnt_flag;
	int id;
	struct fake_mutex *mnt_updating;
	int busy;
	int suspended;
};

struct lwp { void *l_cred; };
struct lwp fake_lwp;
struct lwp *curlwp = &fake_lwp;

struct allocation {
	void *ptr;
	size_t size;
};
static struct allocation allocations[128];
static size_t nallocations;

static struct mount *all_mounts[MAX_MOUNTS];
static size_t nall_mounts;
static size_t iterator_index;
static struct mount *iterator_mount;
static int iterator_live;
static int iterator_skip = -1;
static int busy_failure = -1;
static int update_failure = -1;
static int suspend_failure = -1;
static int suspendctl_failure = -1;
static int sync_failure = -1;
static int fake_global_held;
static int resume_count[MAX_MOUNTS];
static int event_kind[MAX_EVENTS];
static int event_mount[MAX_EVENTS];
static size_t nevents;

enum { EVENT_SUSPEND, EVENT_SYNC, EVENT_RESUME };

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	exit(EXIT_FAILURE);
}

static void
assert_mounts_excluded(void)
{
	if (vfs_s2idle_mount_enter() != EBUSY)
		fail("mount operation admitted inside barrier");
}

static void
record_event(int kind, int id)
{
	assert_mounts_excluded();
	if (nevents == MAX_EVENTS)
		fail("event log overflow");
	event_kind[nevents] = kind;
	event_mount[nevents++] = id;
}

void *
kmem_alloc(size_t size, int flags)
{
	(void)flags;
	void *p = malloc(size == 0 ? 1 : size);
	if (p == NULL)
		fail("malloc");
	if (nallocations == sizeof(allocations) / sizeof(allocations[0]))
		fail("allocation log overflow");
	allocations[nallocations++] = (struct allocation){ p, size };
	return p;
}

void
kmem_free(void *ptr, size_t size)
{
	for (size_t i = 0; i < nallocations; i++) {
		if (allocations[i].ptr != ptr)
			continue;
		if (allocations[i].size != size)
			fail("kmem_free size did not match allocation capacity");
		free(ptr);
		allocations[i] = allocations[--nallocations];
		return;
	}
	fail("kmem_free unknown pointer");
}

void
mountlist_iterator_init(mount_iterator_t **ip)
{
	assert_mounts_excluded();
	if (iterator_live)
		fail("iterator initialized twice");
	iterator_live = 1;
	iterator_mount = NULL;
	iterator_index = 0;
	*ip = (mount_iterator_t *)(uintptr_t)1;
}

void
mountlist_iterator_destroy(mount_iterator_t *iter)
{
	(void)iter;
	if (!iterator_live)
		fail("iterator destroyed twice");
	if (iterator_mount != NULL)
		vfs_unbusy(iterator_mount);
	iterator_mount = NULL;
	iterator_live = 0;
}

struct mount *
mountlist_iterator_trynext_ex(mount_iterator_t *iter, int *skippedp)
{
	(void)iter;
	if (!iterator_live)
		fail("iteration after destruction");
	if (iterator_mount != NULL)
		vfs_unbusy(iterator_mount);
	iterator_mount = NULL;
	*skippedp = 0;
	while (iterator_index != nall_mounts) {
		struct mount *mp = all_mounts[iterator_index++];
		if (mp->id == iterator_skip) {
			*skippedp = 1;
			continue;
		}
		/* The native iterator owns a busy hold until next/destroy. */
		mp->busy++;
		iterator_mount = mp;
		return mp;
	}
	return NULL;
}

struct mount *
mountlist_iterator_trynext(mount_iterator_t *iter)
{
	int skipped;

	return mountlist_iterator_trynext_ex(iter, &skipped);
}

int
vfs_trybusy(struct mount *mp)
{
	if (mp->id == busy_failure)
		return EBUSY;
	mp->busy++;
	return 0;
}

void
vfs_unbusy(struct mount *mp)
{
	assert_mounts_excluded();
	if (mp->busy <= 0)
		fail("busy hold rollback mismatch");
	mp->busy--;
}

void
mutex_init(struct fake_mutex *mtx, int type, int ipl)
{
	(void)type;
	(void)ipl;
	mtx->held = 0;
	mtx->id = -2;
	if (pthread_mutex_init(&mtx->native, NULL) != 0)
		fail("pthread_mutex_init");
}

void
mutex_enter(struct fake_mutex *mtx)
{
	if (pthread_mutex_lock(&mtx->native) != 0)
		fail("pthread_mutex_lock");
	if (mtx->held)
		fail("recursive gate mutex acquisition");
	mtx->held = 1;
}

int
mutex_tryenter(struct fake_mutex *mtx)
{
	if (mtx == NULL || mtx->held || mtx->id == update_failure)
		return 0;
	mtx->held = 1;
	return 1;
}

void
mutex_exit(struct fake_mutex *mtx)
{
	if (mtx == NULL || !mtx->held)
		fail("mutex rollback mismatch");
	mtx->held = 0;
	if (mtx->id == -2 && pthread_mutex_unlock(&mtx->native) != 0)
		fail("pthread_mutex_unlock");
}

int
fake_suspendctl(struct mount *mp, int action)
{
	if (action == SUSPEND_SUSPEND) {
		if (mp->id == suspend_failure || mp->id == suspendctl_failure)
			return EIO;
		if (mp->suspended)
			fail("duplicate suspend");
		mp->suspended = 1;
		record_event(EVENT_SUSPEND, mp->id);
		return 0;
	}
	if (action == SUSPEND_RESUME) {
		if (!mp->suspended || resume_count[mp->id] != 0)
			fail("double or missing resume");
		mp->suspended = 0;
		resume_count[mp->id]++;
		record_event(EVENT_RESUME, mp->id);
		return 0;
	}
	return EINVAL;
}

int
vfs_suspend(struct mount *mp, int nowait)
{
	if (!nowait || fake_global_held)
		fail("suspend was not a nonblocking first admission");
	if (mp->id == suspend_failure)
		return EIO;
	if (fake_suspendctl(mp, SUSPEND_SUSPEND) != 0)
		return EIO;
	fake_global_held = 1;
	return 0;
}

void
vfs_resume(struct mount *mp)
{
	if (!fake_global_held)
		fail("global suspension lock was not held");
	if (fake_suspendctl(mp, SUSPEND_RESUME) != 0)
		fail("global resume failed");
	fake_global_held = 0;
}

int
fake_sync(struct mount *mp, int flags, void *cred)
{
	(void)flags;
	(void)cred;
	if (mp->backing != NULL && mp->backing->suspended)
		fail("sync attempted through a suspended backing filesystem");
	if (mp->id == sync_failure)
		return EIO;
	record_event(EVENT_SYNC, mp->id);
	return 0;
}

#include "../kernel/kern/vfs_s2idle.c"

static void
reset_case(size_t count)
{
	static struct mount mounts[MAX_MOUNTS];
	static struct fake_mutex mutexes[MAX_MOUNTS];

	if (count > MAX_MOUNTS)
		fail("test mount count overflow");
	memset(mounts, 0, sizeof(mounts));
	memset(mutexes, 0, sizeof(mutexes));
	memset(resume_count, 0, sizeof(resume_count));
	memset(event_kind, 0, sizeof(event_kind));
	memset(event_mount, 0, sizeof(event_mount));
	nall_mounts = count;
	nevents = 0;
	nallocations = 0;
	iterator_index = 0;
	iterator_skip = -1;
	busy_failure = update_failure = suspend_failure = -1;
	suspendctl_failure = sync_failure = -1;
	fake_global_held = 0;
	for (size_t i = 0; i < count; i++) {
		mounts[i].id = (int)i;
		mounts[i].mnt_updating = &mutexes[i];
		mutexes[i].id = (int)i;
		all_mounts[i] = &mounts[i];
	}
}

static void
assert_events(const int *kinds, const int *ids, size_t n)
{
	if (nevents != n)
		fail("unexpected event count");
	for (size_t i = 0; i < n; i++) {
		if (event_kind[i] != kinds[i] || event_mount[i] != ids[i]) {
			fprintf(stderr, "event mismatch at %zu: got %d/%d expected %d/%d\\n",
			    i, event_kind[i], event_mount[i], kinds[i], ids[i]);
			fail("unexpected event order");
		}
	}
}

static void
assert_clean(void)
{
	if (vfs_s2idle_owner != NULL || vfs_s2idle_mountops != 0 ||
	    vfs_s2idle_lock.held)
		fail("mount admission gate leaked");
	if (fake_global_held || nallocations != 0 || iterator_live)
		fail("session or allocation leaked");
	for (size_t i = 0; i < nall_mounts; i++) {
		if (all_mounts[i]->busy || all_mounts[i]->suspended ||
		    all_mounts[i]->mnt_updating->held)
			fail("mount state leaked");
	}
}

static void
test_success(size_t count)
{
	struct vfs_s2idle_mount *session;
	size_t nsession;
	int kinds[MAX_EVENTS], ids[MAX_EVENTS];
	size_t n = 0;

	reset_case(count);
	if (vfs_suspend_all(&session, &nsession) != 0 || nsession != count)
		fail("successful session was rejected");
	for (size_t i = 0; i < count; i++) {
		kinds[n] = EVENT_SUSPEND;
		ids[n++] = (int)(count - i - 1);
		kinds[n] = EVENT_SYNC;
		ids[n++] = (int)(count - i - 1);
	}
	assert_mounts_excluded();
	vfs_resume_all(session, nsession);
	for (size_t i = 0; i < count; i++) {
		kinds[n] = EVENT_RESUME;
		ids[n++] = (int)i;
	}
	assert_events(kinds, ids, n);
	assert_clean();
}

static void
test_duplicate_lower(void)
{
	struct vfs_s2idle_mount *session;
	size_t nsession;
	int kinds[] = { EVENT_SUSPEND, EVENT_SYNC, EVENT_SYNC,
	    EVENT_SUSPEND, EVENT_SYNC, EVENT_RESUME, EVENT_RESUME };
	int ids[] = { 1, 2, 1, 0, 0, 0, 1 };

	reset_case(3);
	all_mounts[2]->mnt_lower = all_mounts[1];
	if (vfs_suspend_all(&session, &nsession) != 0)
		fail("layered session was rejected");
	vfs_resume_all(session, nsession);
	assert_events(kinds, ids, sizeof(kinds) / sizeof(kinds[0]));
	assert_clean();
}

static void
test_backing_alias(void)
{
	struct vfs_s2idle_mount *session;
	size_t nsession;

	reset_case(4);
	all_mounts[2]->backing = all_mounts[1];
	/* A later null mount must not move its backing mount ahead of vnd. */
	all_mounts[3]->mnt_lower = all_mounts[1];
	all_mounts[3]->mnt_updating = all_mounts[1]->mnt_updating;
	if (vfs_suspend_all(&session, &nsession) != 0)
		fail("backing alias session rejected");
	vfs_resume_all(session, nsession);
	assert_clean();
}


static void
test_failures(void)
{
	struct vfs_s2idle_mount *session;
	size_t nsession;

	reset_case(3);
	iterator_skip = 1;
	if (vfs_suspend_all(&session, &nsession) != EBUSY)
		fail("iterator skip was not rejected");
	assert_clean();

	reset_case(3);
	busy_failure = 1;
	if (vfs_suspend_all(&session, &nsession) != EBUSY)
		fail("busy admission did not fail");
	assert_clean();

	reset_case(3);
	suspend_failure = 2;
	if (vfs_suspend_all(&session, &nsession) != EIO)
		fail("global suspend failure did not fail");
	assert_clean();

	reset_case(3);
	suspendctl_failure = 1;
	if (vfs_suspend_all(&session, &nsession) != EIO)
		fail("direct suspend failure did not fail");
	if (nevents != 3 || event_kind[0] != EVENT_SUSPEND ||
	    event_kind[2] != EVENT_RESUME || event_mount[2] != 2)
		fail("direct suspend rollback order");
	assert_clean();

	reset_case(3);
	sync_failure = 1;
	if (vfs_suspend_all(&session, &nsession) != EIO)
		fail("sync failure did not fail");
	if (nevents != 5 || event_kind[3] != EVENT_RESUME ||
	    event_mount[3] != 1 || event_kind[4] != EVENT_RESUME ||
	    event_mount[4] != 2)
		fail("sync rollback or resume order");
	assert_clean();

	reset_case(3);
	update_failure = 1;
	if (vfs_suspend_all(&session, &nsession) != EBUSY)
		fail("updater admission did not fail");
	assert_clean();

	reset_case(3);
	all_mounts[2]->mnt_lower = all_mounts[1];
	all_mounts[1]->mnt_lower = all_mounts[2];
	if (vfs_suspend_all(&session, &nsession) != ELOOP)
		fail("lower-mount cycle did not fail");
	assert_clean();
}

static void
test_mount_admission(void)
{
	struct vfs_s2idle_mount *session, *other;
	size_t nsession, nother;

	reset_case(3);
	if (vfs_s2idle_mount_enter() != 0 || vfs_s2idle_mount_enter() != 0)
		fail("nested mount operation rejected");
	if (vfs_suspend_all(&session, &nsession) != EBUSY ||
	    session != NULL || nsession != 0 || iterator_live || nevents != 0)
		fail("active mount operation did not prevent barrier admission");
	vfs_s2idle_mount_exit();
	if (vfs_suspend_all(&session, &nsession) != EBUSY)
		fail("outer mount operation lost its admission");
	vfs_s2idle_mount_exit();
	if (vfs_suspend_all(&session, &nsession) != 0)
		fail("completed mount operation still prevents barrier");
	assert_mounts_excluded();
	if (vfs_suspend_all(&other, &nother) != EBUSY ||
	    other != NULL || nother != 0)
		fail("second barrier admitted");
	assert_mounts_excluded();
	vfs_resume_all(session, nsession);
	if (vfs_s2idle_mount_enter() != 0)
		fail("mount operation rejected after resume");
	vfs_s2idle_mount_exit();
	assert_clean();
}

static atomic_bool mount_inside;

static void *
mount_racer(void *arg)
{
	(void)arg;
	for (unsigned i = 0; i < 4000; i++) {
		int error = vfs_s2idle_mount_enter();
		if (error == EBUSY) {
			sched_yield();
			continue;
		}
		if (error != 0)
			fail("concurrent mount admission error");
		atomic_store(&mount_inside, true);
		sched_yield();
		atomic_store(&mount_inside, false);
		vfs_s2idle_mount_exit();
	}
	return NULL;
}

static void
test_concurrent_admission(void)
{
	pthread_t thread;
	struct vfs_s2idle_mount *session;
	size_t nsession;

	reset_case(0);
	if (pthread_create(&thread, NULL, mount_racer, NULL) != 0)
		fail("pthread_create");
	for (unsigned i = 0; i < 4000; i++) {
		int error = vfs_suspend_all(&session, &nsession);
		if (error == EBUSY) {
			sched_yield();
			continue;
		}
		if (error != 0 || atomic_load(&mount_inside))
			fail("barrier overlaps a mount operation");
		sched_yield();
		if (atomic_load(&mount_inside))
			fail("mount entered after barrier admission");
		vfs_resume_all(session, nsession);
	}
	if (pthread_join(thread, NULL) != 0)
		fail("pthread_join");
	assert_clean();
}

int
main(void)
{
	struct vfs_s2idle_mount *session;
	size_t nsession;

	if (vfs_suspend_all(&session, &nsession) != ENXIO ||
	    session != NULL || nsession != 0 ||
	    vfs_s2idle_mount_enter() != ENXIO)
		fail("uninitialized admission hooks accepted");
	vfs_s2idle_init();
	test_success(0);
	test_mount_admission();
	test_success(1);
	test_success(3);
	test_success(17);
	test_duplicate_lower();
	test_backing_alias();
	test_failures();
	test_concurrent_admission();
	puts("s2idle behavioral protocol checks passed");
	return 0;
}
