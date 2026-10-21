/*
 * prime-fuzz [OPS [SEED]]: PRIME identity and lifetime fuzzer for i915.
 *
 * Several DRM files create tagged GEM buffers, export them as dma-bufs, import
 * the dma-bufs into each other and close handles and fds in random orders.
 * Every import reads the buffer back through the new handle and checks its
 * tag: a handle resolving to the wrong buffer (a stale PRIME cache entry, a
 * reused handle) is a mismatch.  At the end everything is closed and the wired
 * memory delta is printed, so the same run checks for leaks.
 *
 * cc -O2 -Wall -I/usr/X11R7/include/libdrm -o prime-fuzz prime-fuzz.c
 * Exit status 1 on any mismatch or unexpected ioctl failure.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <i915_drm.h>

#define NFILES	3
#define NSLOTS	64
#define BOSIZE	(64 * 1024)

struct handle {
	int used;
	uint32_t h;
	uint32_t tag;
};

struct dbuf {
	int used;
	int fd;
	uint32_t tag;
};

static int files[NFILES];
static struct handle handles[NFILES][NSLOTS];
static struct dbuf dbufs[NSLOTS];
static uint32_t nexttag = 1;
static int failures;
static int nexports;
static unsigned char exported[1 << 20];
static int ndistinct;

static long long
wired_bytes(void)
{
	struct uvmexp_sysctl u;
	size_t len = sizeof(u);
	int mib[2] = { CTL_VM, VM_UVMEXP2 };

	if (sysctl(mib, 2, &u, &len, NULL, 0) == -1)
		err(2, "sysctl");
	return (long long)u.wired * u.pagesize;
}

static void
fail(const char *what, int f, uint32_t h)
{
	printf("FAIL: %s (file %d handle %u errno %d)\n", what, f, h, errno);
	failures++;
}

static uint32_t
read_tag(int f, uint32_t h)
{
	struct drm_i915_gem_pread pr;
	uint32_t tag = 0;

	memset(&pr, 0, sizeof(pr));
	pr.handle = h;
	pr.size = sizeof(tag);
	pr.data_ptr = (uintptr_t)&tag;
	if (ioctl(files[f], DRM_IOCTL_I915_GEM_PREAD, &pr) == -1)
		fail("pread", f, h);
	return tag;
}

static void
op_create(int f, int s)
{
	struct drm_i915_gem_create c = { .size = BOSIZE };
	struct drm_i915_gem_pwrite pw;
	struct drm_i915_gem_set_domain dom;
	uint32_t tag = nexttag++;

	if (ioctl(files[f], DRM_IOCTL_I915_GEM_CREATE, &c) == -1) {
		fail("create", f, 0);
		return;
	}
	memset(&pw, 0, sizeof(pw));
	pw.handle = c.handle;
	pw.size = sizeof(tag);
	pw.data_ptr = (uintptr_t)&tag;
	if (ioctl(files[f], DRM_IOCTL_I915_GEM_PWRITE, &pw) == -1)
		fail("pwrite", f, c.handle);
	memset(&dom, 0, sizeof(dom));
	dom.handle = c.handle;
	dom.read_domains = I915_GEM_DOMAIN_GTT;
	if (ioctl(files[f], DRM_IOCTL_I915_GEM_SET_DOMAIN, &dom) == -1)
		fail("set_domain", f, c.handle);
	handles[f][s] = (struct handle){ 1, c.handle, tag };
}

static void
op_export(int f, int s, int d)
{
	struct drm_prime_handle p;

	memset(&p, 0, sizeof(p));
	p.handle = handles[f][s].h;
	p.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(files[f], DRM_IOCTL_PRIME_HANDLE_TO_FD, &p) == -1) {
		fail("export", f, p.handle);
		return;
	}
	dbufs[d] = (struct dbuf){ 1, p.fd, handles[f][s].tag };
	nexports++;
	if (!exported[handles[f][s].tag]) {
		exported[handles[f][s].tag] = 1;
		ndistinct++;
	}
}

static void
op_import(int f, int d)
{
	struct drm_prime_handle p;
	uint32_t tag;
	int s;

	memset(&p, 0, sizeof(p));
	p.fd = dbufs[d].fd;
	if (ioctl(files[f], DRM_IOCTL_PRIME_FD_TO_HANDLE, &p) == -1) {
		fail("import", f, 0);
		return;
	}
	tag = read_tag(f, p.handle);
	if (tag != dbufs[d].tag) {
		printf("FAIL: import into file %d gave handle %u holding tag %u, "
		    "dma-buf has tag %u\n", f, p.handle, tag, dbufs[d].tag);
		failures++;
	}
	/* The kernel returns the existing handle when the file has one. */
	for (s = 0; s < NSLOTS; s++)
		if (handles[f][s].used && handles[f][s].h == p.handle) {
			if (handles[f][s].tag != dbufs[d].tag) {
				printf("FAIL: file %d handle %u is tag %u in "
				    "our books, import says tag %u\n", f,
				    p.handle, handles[f][s].tag, dbufs[d].tag);
				failures++;
			}
			return;
		}
	for (s = 0; s < NSLOTS; s++)
		if (!handles[f][s].used) {
			handles[f][s] = (struct handle){ 1, p.handle,
			    dbufs[d].tag };
			return;
		}
	/* No room: drop it again. */
	{
		struct drm_gem_close gc = { .handle = p.handle };
		ioctl(files[f], DRM_IOCTL_GEM_CLOSE, &gc);
	}
}

static void
op_close_handle(int f, int s)
{
	struct drm_gem_close gc = { .handle = handles[f][s].h };

	if (read_tag(f, handles[f][s].h) != handles[f][s].tag) {
		printf("FAIL: file %d handle %u lost its contents (want tag %u)\n",
		    f, handles[f][s].h, handles[f][s].tag);
		failures++;
	}
	if (ioctl(files[f], DRM_IOCTL_GEM_CLOSE, &gc) == -1)
		fail("gem close", f, gc.handle);
	handles[f][s].used = 0;
}

int
main(int argc, char **argv)
{
	int ops = argc > 1 ? atoi(argv[1]) : 20000;
	long long before, after;
	int i, f, s, d;

	srandom(argc > 2 ? atoi(argv[2]) : 1);
	before = wired_bytes();
	for (f = 0; f < NFILES; f++)
		if ((files[f] = open("/dev/dri/renderD128", O_RDWR)) == -1)
			err(2, "renderD128");
	for (i = 0; i < ops; i++) {
		f = random() % NFILES;
		s = random() % NSLOTS;
		d = random() % NSLOTS;
		switch (random() % 5) {
		case 0:
			if (!handles[f][s].used)
				op_create(f, s);
			break;
		case 1:
			if (handles[f][s].used && !dbufs[d].used)
				op_export(f, s, d);
			break;
		case 2:
			if (dbufs[d].used)
				op_import(f, d);
			break;
		case 3:
			if (handles[f][s].used)
				op_close_handle(f, s);
			break;
		case 4:
			if (dbufs[d].used) {
				close(dbufs[d].fd);
				dbufs[d].used = 0;
			}
			break;
		}
	}
	for (f = 0; f < NFILES; f++)
		for (s = 0; s < NSLOTS; s++)
			if (handles[f][s].used)
				op_close_handle(f, s);
	for (d = 0; d < NSLOTS; d++)
		if (dbufs[d].used)
			close(dbufs[d].fd);
	for (f = 0; f < NFILES; f++)
		close(files[f]);
	sleep(3);
	after = wired_bytes();
	printf("%d ops, %u buffers, %d exports of %d buffers: %d failure(s), wired %+lld KiB "
	    "after closing everything\n", ops, nexttag - 1, nexports, ndistinct, failures,
	    (after - before) / 1024);
	return failures ? 1 : 0;
}
