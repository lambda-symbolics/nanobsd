/*
 * dmabuf-poll-race [ROUNDS] [THREADS]: concurrent pollers on one dma-buf.
 *
 * Each round exports a GEM buffer and starts THREADS threads that poll its
 * fd (zero timeout, POLLIN and POLLOUT alternately) as fast as they can
 * while the main thread submits short batches that write the buffer, so
 * its fence is replaced and signalled all the time and the shared poll
 * callback is claimed, queued, run and reclaimed by different pollers
 * (the kernel 206 dma_resv_poll_arm ordering).  Then the dma-buf is freed
 * with a callback possibly still queued.  It cannot see a corrupted fence
 * queue directly; it is a stress for a DIAGNOSTIC kernel and a smoke test
 * otherwise: the machine must stay up and every poll must return.
 *
 * cc -O2 -Wall -pthread -I/usr/X11R7/include/libdrm -o dmabuf-poll-race \
 *     dmabuf-poll-race.c
 */
#include <sys/ioctl.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm.h>
#include <i915_drm.h>

#define BATCH_DWORDS	2048
#define SUBMITS		200

static int pfdnum;
static atomic_int stop;
static atomic_long polls, notready;

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

static void *
poller(void *arg)
{
	long n = (long)arg;

	while (!atomic_load(&stop)) {
		struct pollfd pfd = {
			.fd = pfdnum,
			.events = (n++ & 1) ? POLLOUT : POLLIN,
		};

		if (poll(&pfd, 1, 0) == 0)
			atomic_fetch_add(&notready, 1);
		atomic_fetch_add(&polls, 1);
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 200;
	int nthreads = argc > 2 ? atoi(argv[2]) : 4;
	pthread_t th[16];
	uint32_t batch, end = 0x0A << 23;	/* MI_BATCH_BUFFER_END */
	struct drm_i915_gem_pwrite pw;
	int fd, i, j;

	if (nthreads < 1 || nthreads > 16)
		errx(2, "THREADS must be 1..16");
	if ((fd = open("/dev/dri/renderD128", O_RDWR)) == -1)
		err(2, "renderD128");

	batch = gem_create(fd, (BATCH_DWORDS + 2) * 4);
	memset(&pw, 0, sizeof(pw));
	pw.handle = batch;
	pw.offset = BATCH_DWORDS * 4;
	pw.size = sizeof(end);
	pw.data_ptr = (uintptr_t)&end;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &pw) == -1)
		err(2, "PWRITE batch");

	for (i = 0; i < rounds; i++) {
		struct drm_prime_handle prime;
		uint32_t target = gem_create(fd, 4096);

		memset(&prime, 0, sizeof(prime));
		prime.handle = target;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == -1)
			err(2, "PRIME_HANDLE_TO_FD");
		pfdnum = prime.fd;

		atomic_store(&stop, 0);
		for (j = 0; j < nthreads; j++)
			if (pthread_create(&th[j], NULL, poller,
			    (void *)(long)j) != 0)
				errx(2, "pthread_create");

		for (j = 0; j < SUBMITS; j++) {
			struct drm_i915_gem_exec_object2 obj[2];
			struct drm_i915_gem_execbuffer2 eb;

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
		}

		atomic_store(&stop, 1);
		for (j = 0; j < nthreads; j++)
			pthread_join(th[j], NULL);

		/* Free the dma-buf, a callback possibly still queued. */
		close(prime.fd);
		gem_close(fd, target);
	}
	gem_close(fd, batch);
	close(fd);
	sleep(1);
	printf("%d rounds x %d threads: %ld polls, %ld found the buffer busy\n",
	    rounds, nthreads, atomic_load(&polls), atomic_load(&notready));
	return atomic_load(&notready) == 0 ? 2 : 0;
}
