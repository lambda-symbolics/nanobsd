/*
 * dmabuf-poll [ROUNDS]: tear down a dma-buf while a poll callback is queued.
 *
 * Each round exports a GEM buffer, submits a batch of no-ops that writes it
 * (its reservation gets the batch's fence), polls the dma-buf fd with a zero
 * timeout while the batch still runs, which queues a callback on the fence
 * inside the dma-buf, and then closes the fd and the handle at once, so the
 * dma-buf is freed with the callback still queued.  Without the
 * dma_resv_poll_fini cancellation that is a use after free (or a KASSERT on
 * a DIAGNOSTIC kernel) when the batch completes.  Prints how many rounds
 * really caught the batch still running.
 *
 * cc -O2 -Wall -I/usr/X11R7/include/libdrm -o dmabuf-poll dmabuf-poll.c
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <i915_drm.h>

#define BATCH_DWORDS	(64 * 1024)	/* MI_NOOP is 0 */

static uint32_t
gem_create(int fd, uint64_t size)
{
	struct drm_i915_gem_create c = { .size = size };

	if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &c) == -1)
		err(2, "GEM_CREATE");
	return c.handle;
}

static void
gem_close(int fd, uint32_t h)
{
	struct drm_gem_close gc = { .handle = h };

	if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == -1)
		err(2, "GEM_CLOSE");
}

int
main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 2000;
	int fd, i, pending = 0;
	uint32_t batch, end = 0x0A << 23;	/* MI_BATCH_BUFFER_END */
	struct drm_i915_gem_pwrite pw;

	if ((fd = open("/dev/dri/renderD128", O_RDWR)) == -1)
		err(2, "renderD128");

	/* One batch, reused: BATCH_DWORDS no-ops then the end. */
	batch = gem_create(fd, (BATCH_DWORDS + 2) * 4);
	memset(&pw, 0, sizeof(pw));
	pw.handle = batch;
	pw.offset = BATCH_DWORDS * 4;
	pw.size = sizeof(end);
	pw.data_ptr = (uintptr_t)&end;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &pw) == -1)
		err(2, "PWRITE batch");

	for (i = 0; i < rounds; i++) {
		struct drm_i915_gem_exec_object2 obj[2];
		struct drm_i915_gem_execbuffer2 eb;
		struct drm_prime_handle prime;
		struct pollfd pfd;
		uint32_t target = gem_create(fd, 4096);

		memset(&prime, 0, sizeof(prime));
		prime.handle = target;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == -1)
			err(2, "PRIME_HANDLE_TO_FD");

		memset(obj, 0, sizeof(obj));
		obj[0].handle = target;
		obj[0].flags = EXEC_OBJECT_WRITE;
		obj[1].handle = batch;
		memset(&eb, 0, sizeof(eb));
		eb.buffers_ptr = (uintptr_t)obj;
		eb.buffer_count = 2;
		eb.batch_len = (BATCH_DWORDS + 2) * 4;
		eb.flags = I915_EXEC_RENDER;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb) == -1)
			err(2, "EXECBUFFER2");

		/* POLLIN waits for the exclusive (write) fence. */
		pfd.fd = prime.fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) == 0)
			pending++;

		/* Free the dma-buf now, callback (if any) still queued. */
		close(prime.fd);
		gem_close(fd, target);
	}
	gem_close(fd, batch);
	close(fd);
	sleep(1);
	printf("%d rounds, %d polled while the batch was still running\n",
	    rounds, pending);
	return pending == 0 ? 2 : 0;
}
