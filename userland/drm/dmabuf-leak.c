/*
 * dmabuf-leak [ITERATIONS [KIB]]: does closing an exported GEM buffer free it?
 *
 * Each round creates an i915 GEM buffer, gives it pages (a GTT set-domain),
 * exports
 * it as a dma-buf (PRIME, as Mesa does for every Wayland buffer and VA-API
 * frame), then closes the dma-buf fd and the GEM handle.  Nothing refers to
 * the buffer afterwards, so the kernel must free it.  NetBSD 11.0 never
 * calls the dma-buf release hook, so every round pinned its pages for good
 * (2026-10-19: 6.8 GB wired after 2.6 h of Firefox, Firefox shm failures).
 *
 * cc -O2 -Wall -I/usr/X11R7/include/libdrm -o dmabuf-leak dmabuf-leak.c
 * Prints the change in wired memory; exit status 1 when it grew by more than
 * a tenth of what the rounds allocated.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <i915_drm.h>

static long long
wired_bytes(void)
{
	struct uvmexp_sysctl u;
	size_t len = sizeof(u);
	int mib[2] = { CTL_VM, VM_UVMEXP2 };

	if (sysctl(mib, 2, &u, &len, NULL, 0) == -1)
		err(2, "sysctl vm.uvmexp2");
	return (long long)u.wired * u.pagesize;
}

int
main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 100;
	size_t size = (size_t)(argc > 2 ? atoi(argv[2]) : 1024) * 1024;
	char *data = calloc(1, size);
	long long before, after, grown;
	int fd, i;

	if ((fd = open("/dev/dri/renderD128", O_RDWR)) == -1)
		err(2, "/dev/dri/renderD128");
	memset(data, 0x5a, size);
	before = wired_bytes();
	for (i = 0; i < rounds; i++) {
		struct drm_i915_gem_create create = { .size = size };
		struct drm_i915_gem_pwrite pw;
		struct drm_i915_gem_set_domain dom;
		struct drm_prime_handle prime;
		struct drm_gem_close gclose;

		if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) == -1)
			err(2, "GEM_CREATE");
		memset(&pw, 0, sizeof(pw));
		pw.handle = create.handle;
		pw.size = size;
		pw.data_ptr = (uintptr_t)data;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &pw) == -1)
			err(2, "GEM_PWRITE");
		/*
		 * Reading through the GTT makes i915 acquire the object's pages
		 * (a new object is already in the CPU domain, so asking for that
		 * returns early and leaves it without pages).
		 */
		memset(&dom, 0, sizeof(dom));
		dom.handle = create.handle;
		dom.read_domains = I915_GEM_DOMAIN_GTT;
		dom.write_domain = 0;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &dom) == -1)
			err(2, "GEM_SET_DOMAIN");
		memset(&prime, 0, sizeof(prime));
		prime.handle = create.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == -1)
			err(2, "PRIME_HANDLE_TO_FD");
		close(prime.fd);
		memset(&gclose, 0, sizeof(gclose));
		gclose.handle = create.handle;
		if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gclose) == -1)
			err(2, "GEM_CLOSE");
	}
	close(fd);
	/* i915 frees objects from a worker; give it a moment. */
	sleep(2);
	after = wired_bytes();
	grown = after - before;
	printf("%d rounds of %zu KiB exported and closed: wired %+lld KiB\n",
	    rounds, size / 1024, grown / 1024);
	if (grown > (long long)rounds * (long long)size / 10) {
		printf("LEAK: closed dma-buf buffers stay allocated\n");
		return 1;
	}
	printf("ok\n");
	return 0;
}
