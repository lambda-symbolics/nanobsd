#!/bin/sh
# Behavioral host protocol checks for the selective-s2idle filesystem barrier.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/s2idle.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/sys"

cat >"$tmp/test-kernel.h" <<'EOF'
#ifndef S2IDLE_TEST_KERNEL_H
#define S2IDLE_TEST_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct fake_mutex kmutex_t;
typedef struct fake_iterator mount_iterator_t;
struct mount;
struct lwp;
extern struct lwp *curlwp;

#define __KERNEL_RCSID(n, s)
#define KASSERT(x) do { if (!(x)) abort(); } while (0)
#define KM_SLEEP 0
#define MNT_ASYNC 0x01
#define MNT_WAIT 0x02
#define SUSPEND_SUSPEND 1
#define SUSPEND_RESUME 2
#define VFS_SUSPENDCTL(mp, action) fake_suspendctl((mp), (action))
#define VFS_SYNC(mp, flags, cred) fake_sync((mp), (flags), (cred))

void *kmem_alloc(size_t, int);
void kmem_free(void *, size_t);
int fake_suspendctl(struct mount *, int);
int fake_sync(struct mount *, int, void *);
int mutex_tryenter(kmutex_t *);
void mutex_exit(kmutex_t *);
int vfs_trybusy(struct mount *);
void vfs_unbusy(struct mount *);
int vfs_suspend(struct mount *, int);
void vfs_resume(struct mount *);
void mountlist_iterator_destroy(mount_iterator_t *);
struct mount *mountlist_iterator_trynext_ex(mount_iterator_t *, int *);
struct mount *mountlist_iterator_trynext(mount_iterator_t *);
#endif
EOF

cat >"$tmp/sys/cdefs.h" <<'EOF'
#include_next <sys/cdefs.h>
#ifndef __KERNEL_RCSID
#define __KERNEL_RCSID(n, s)
#endif
EOF
for h in param.h errno.h fstrans.h kmem.h lwp.h mount.h mutex.h systm.h; do
    printf '#include "../test-kernel.h"\n' >"$tmp/sys/$h"
done
cat >"$tmp/sys/types.h" <<'EOF'
#include "../test-kernel.h"
EOF
cat >"$tmp/sys/vfs_s2idle.h" <<EOF
#include "$root/kernel/sys/vfs_s2idle.h"
EOF

cc -std=c11 -Wall -Wextra -Werror -D_KERNEL -I"$tmp" \
    "$root/tests/s2idle-harness.c" -o "$tmp/s2idle-harness"
"$tmp/s2idle-harness"
