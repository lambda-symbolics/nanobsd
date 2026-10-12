/*
 * bootsplash [-f] [-t /dev/ttyE0]: the LispBSD lambda on the console
 * framebuffer while rc runs.  Puts the wsdisplay screen into dumb
 * framebuffer mode, paints, and waits; on SIGTERM/SIGINT/SIGHUP it puts
 * the screen back into text mode, clears it, and exits.  Daemonises
 * unless -f.  Pid in /var/run/bootsplash.pid.
 *
 * Build: cc -O2 -o bootsplash bootsplash.c (logo.h is generated from
 * lfetch's logo.lisp by mkbitmap.py).  LispBSD, 2026-10-13.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dev/wscons/wsconsio.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logo.h"

static volatile sig_atomic_t done;
static void stop(int sig) { (void)sig; done = 1; }

/* lfetch's six greens, top to bottom (xterm 157 121 85 84 78 72). */
static const uint8_t shades[6][3] = {
	{175, 255, 175}, {135, 255, 175}, {95, 255, 175},
	{95, 255, 135},  {95, 215, 135},  {95, 175, 135},
};

static void
put(uint8_t *fb, u_int stride, u_int depth, u_int x, u_int y,
    uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *p = fb + (size_t)y * stride + (size_t)x * (depth / 8);
	if (depth == 32) {
		p[0] = b; p[1] = g; p[2] = r; p[3] = 0;
	} else if (depth == 16) {
		uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		memcpy(p, &v, 2);
	}
}

int
main(int argc, char **argv)
{
	const char *tty = "/dev/ttyE0";
	int foreground = 0, ch, fd, mode;
	struct wsdisplay_fbinfo info;
	u_int stride;
	uint8_t *fb;
	size_t size;

	while ((ch = getopt(argc, argv, "ft:")) != -1) {
		switch (ch) {
		case 'f': foreground = 1; break;
		case 't': tty = optarg; break;
		default: fprintf(stderr, "usage: bootsplash [-f] [-t tty]\n"); return 2;
		}
	}
	fd = open(tty, O_RDWR);
	if (fd < 0)
		err(1, "%s", tty);
	if (ioctl(fd, WSDISPLAYIO_GINFO, &info) < 0)
		err(1, "WSDISPLAYIO_GINFO");
	if (ioctl(fd, WSDISPLAYIO_LINEBYTES, &stride) < 0)
		err(1, "WSDISPLAYIO_LINEBYTES");
	if (info.depth != 32 && info.depth != 16)
		errx(1, "depth %u not handled", info.depth);
	size = (size_t)stride * info.height;

	if (!foreground) {
		if (daemon(0, 0) < 0)
			err(1, "daemon");
	}
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGHUP, stop);

	mode = WSDISPLAYIO_MODE_DUMBFB;
	if (ioctl(fd, WSDISPLAYIO_SMODE, &mode) < 0)
		err(1, "WSDISPLAYIO_SMODE dumbfb");
	fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		mode = WSDISPLAYIO_MODE_EMUL;
		ioctl(fd, WSDISPLAYIO_SMODE, &mode);
		err(1, "mmap");
	}
	{
		FILE *pf = fopen("/var/run/bootsplash.pid", "w");
		if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
	}

	/* Black, then the lambda scaled to a third of the screen height. */
	memset(fb, 0, size);
	{
		u_int scale = info.height / 3 / LOGO_H;
		u_int w, h, x0, y0, x, y, sx, sy, band;
		if (scale < 1) scale = 1;
		w = LOGO_W * scale; h = LOGO_H * scale;
		x0 = (info.width - w) / 2; y0 = (info.height - h) / 2;
		for (y = 0; y < LOGO_H; y++) {
			band = y * 6 / LOGO_H;
			for (x = 0; x < LOGO_W; x++) {
				if (!logo_bits[y][x]) continue;
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++)
						put(fb, stride, info.depth,
						    x0 + x * scale + sx, y0 + y * scale + sy,
						    shades[band][0], shades[band][1], shades[band][2]);
			}
		}
	}

	while (!done)
		pause();

	munmap(fb, size);
	mode = WSDISPLAYIO_MODE_EMUL;
	ioctl(fd, WSDISPLAYIO_SMODE, &mode);
	/* the text screen comes back with the old lines; start it clean */
	(void)write(fd, "\033[H\033[2J", 7);
	close(fd);
	unlink("/var/run/bootsplash.pid");
	return 0;
}
