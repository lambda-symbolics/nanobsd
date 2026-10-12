/*
 * bootsplash [-f] [-t /dev/ttyE0] [-m message]: the LispBSD lambda on the
 * console framebuffer while rc runs, a message in ProFont under it and a
 * braille spinner turning beside it.  Puts the wsdisplay screen into dumb
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

#include <time.h>

#include "font.h"
#include "logo.h"

static volatile sig_atomic_t done;
static void stop(int sig) { (void)sig; done = 1; }

/* The braille spinner, cells as dot bitmaps: bit n = braille dot n+1
 * (dots 1-3 left column top-down, 4-6 right column, 7-8 bottom row). */
static const uint8_t spinner[] = { 0x0b, 0x19, 0x39, 0x38, 0x3c, 0x34, 0x26, 0x27, 0x07, 0x0f };
static const int dotx[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
static const int doty[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };

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

static void
box(uint8_t *fb, u_int stride, u_int depth, u_int x, u_int y, u_int w,
    u_int h, uint8_t r, uint8_t g, uint8_t b)
{
	u_int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			put(fb, stride, depth, x + i, y + j, r, g, b);
}

static void
text(uint8_t *fb, u_int stride, u_int depth, u_int x, u_int y, u_int scale,
    const char *msg, const uint8_t *rgb)
{
	u_int col, row;
	for (; *msg != '\0'; msg++, x += FONT_W * scale) {
		unsigned char c = (unsigned char)*msg;
		if (c < 32 || c > 126)
			continue;
		for (row = 0; row < FONT_H; row++)
			for (col = 0; col < FONT_W; col++)
				if (font_bits[c - 32][row] & (1u << col))
					box(fb, stride, depth, x + col * scale,
					    y + row * scale, scale, scale,
					    rgb[0], rgb[1], rgb[2]);
	}
}

static void
spin(uint8_t *fb, u_int stride, u_int depth, u_int x, u_int y, u_int dot,
    unsigned frame, const uint8_t *rgb)
{
	int n;
	uint8_t cell = spinner[frame % (sizeof(spinner) / sizeof(spinner[0]))];
	box(fb, stride, depth, x, y, 2 * dot + dot / 2, 4 * dot + 3 * (dot / 2), 0, 0, 0);
	for (n = 0; n < 8; n++)
		if (cell & (1 << n))
			box(fb, stride, depth, x + dotx[n] * (dot + dot / 2),
			    y + doty[n] * (dot + dot / 2), dot, dot,
			    rgb[0], rgb[1], rgb[2]);
}

int
main(int argc, char **argv)
{
	const char *tty = "/dev/ttyE0";
	const char *msg = "LispBSD is waking up";
	int foreground = 0, ch, fd, mode;
	u_int spin_x = 0, spin_y = 0, spin_dot = 1;
	unsigned frame = 0;
	struct wsdisplay_fbinfo info;
	u_int stride;
	uint8_t *fb;
	size_t size;

	while ((ch = getopt(argc, argv, "fm:t:")) != -1) {
		switch (ch) {
		case 'f': foreground = 1; break;
		case 'm': msg = optarg; break;
		case 't': tty = optarg; break;
		default: fprintf(stderr, "usage: bootsplash [-f] [-m message] [-t tty]\n"); return 2;
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

	/*
	 * Under the lambda: the spinner, a gap, the message; text at a third
	 * of the lambda's dot size, the spinner's dots a quarter of a text
	 * line tall so it sits on the same line.
	 */
	{
		u_int scale = info.height / 3 / LOGO_H;
		u_int tscale, tw, th, lw, x, y, gap;
		if (scale < 1) scale = 1;
		tscale = scale / 3; if (tscale < 1) tscale = 1;
		tw = (u_int)strlen(msg) * FONT_W * tscale; th = FONT_H * tscale;
		spin_dot = th / 6; if (spin_dot < 2) spin_dot = 2;
		gap = FONT_W * tscale;
		lw = 2 * spin_dot + spin_dot / 2 + gap + tw;
		x = (info.width - lw) / 2;
		y = (info.height + LOGO_H * scale) / 2 + th;
		spin_x = x; spin_y = y + (th - (4 * spin_dot + 3 * (spin_dot / 2))) / 2;
		text(fb, stride, info.depth, x + 2 * spin_dot + spin_dot / 2 + gap, y,
		    tscale, msg, shades[4]);
	}

	while (!done) {
		struct timespec ts = { 0, 100000000 };
		spin(fb, stride, info.depth, spin_x, spin_y, spin_dot, frame++, shades[1]);
		nanosleep(&ts, NULL);
	}

	munmap(fb, size);
	mode = WSDISPLAYIO_MODE_EMUL;
	ioctl(fd, WSDISPLAYIO_SMODE, &mode);
	/* the text screen comes back with the old lines; start it clean */
	(void)write(fd, "\033[H\033[2J", 7);
	close(fd);
	unlink("/var/run/bootsplash.pid");
	return 0;
}
