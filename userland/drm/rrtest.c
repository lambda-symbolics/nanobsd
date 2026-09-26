/*
 * rrtest: switch the eDP CRTC between its modes and measure the result from
 * the pipe's hardware frame counter. A full modeset disables the pipe, which
 * restarts the counter and takes hundreds of milliseconds on eDP; a seamless
 * switch keeps the counter running and finishes within a frame or two.
 * Needs root (/dev/mem) and DRM master, so no compositor may be running.
 * usage: rrtest HZ [HZ ...]    e.g. rrtest 30 60 30 60
 */
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define BAR0	0x603c000000ULL
static volatile uint32_t *mmio;
static uint32_t rd(uint32_t off) { return mmio[off / 4]; }
static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

int main(int argc, char **argv) {
	int fd = open("/dev/dri/card0", O_RDWR), mfd = open("/dev/mem", O_RDONLY);
	if (fd < 0 || mfd < 0) { perror("open"); return 1; }
	void *m = mmap(0, 0x80000, PROT_READ, MAP_SHARED, mfd, BAR0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	mmio = m;
	if (drmSetMaster(fd)) { perror("drmSetMaster (is a compositor running?)"); return 1; }
	drmModeRes *res = drmModeGetResources(fd);
	drmModeConnector *c = NULL;
	for (int i = 0; i < res->count_connectors && !c; i++) {
		drmModeConnector *x = drmModeGetConnector(fd, res->connectors[i]);
		if (x && x->connection == DRM_MODE_CONNECTED && x->connector_type == DRM_MODE_CONNECTOR_eDP) c = x;
	}
	if (!c) { fprintf(stderr, "no eDP\n"); return 1; }
	drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
	drmModeCrtc *crtc = drmModeGetCrtc(fd, e->crtc_id);
	printf("eDP connector %u crtc %u fb %u current %s@%u\n", c->connector_id, crtc->crtc_id,
	    crtc->buffer_id, crtc->mode.name, crtc->mode.vrefresh);
	uint32_t conn = c->connector_id;
	for (int a = 1; a < argc; a++) {
		int hz = atoi(argv[a]), best = -1;
		for (int i = 0; i < c->count_modes; i++)
			if (best < 0 || abs((int)c->modes[i].vrefresh - hz) < abs((int)c->modes[best].vrefresh - hz)) best = i;
		drmModeModeInfo *mode = &c->modes[best];
		uint32_t c0 = rd(0x70040); double t0 = now();
		int rc = drmModeSetCrtc(fd, crtc->crtc_id, crtc->buffer_id, 0, 0, &conn, 1, mode);
		double t1 = now(); uint32_t c1 = rd(0x70040);
		usleep(200000);
		uint32_t c2 = rd(0x70040); double t2 = now();
		sleep(2);
		uint32_t c3 = rd(0x70040); double t3 = now();
		printf("-> %2u Hz mode (%u kHz): rc %d, ioctl %5.1f ms, counter %s (%u -> %u), measured %.2f Hz, LINK_M1 0x%05x/N 0x%05x\n",
		    mode->vrefresh, mode->clock, rc, (t1 - t0) * 1e3,
		    c1 >= c0 && c1 - c0 < 100 ? "continuous" : "RESTARTED", c0, c1,
		    (c3 - c2) / (t3 - t2), rd(0x60040), rd(0x60044));
	}
	drmModeSetCrtc(fd, crtc->crtc_id, crtc->buffer_id, 0, 0, &conn, 1, &crtc->mode);
	drmDropMaster(fd);
	return 0;
}
