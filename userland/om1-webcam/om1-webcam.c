/*
 * om1-webcam: OM System OM-1 live view as a LispBSD webcam.
 *
 * Talks PTP over USB (libusb-1.0) to the camera, with the vendor init the
 * official OM Webcam software sends (0x9489 and 0x948b property lists,
 * documented in FINDINGS.md of the om-1-webcam project), then pulls
 * MJPEG live-view frames (0x9484) and writes each one to /dev/vcam, the
 * LispBSD virtual camera (vcam(4)), from where Firefox reads it as
 * /dev/videoN.  While no reader streams, the loop sleeps in VCAMIOCWAIT
 * and the camera stops producing frames: nothing polls.
 *
 * An interrupt-endpoint drain thread runs throughout: the OM-1 blocks
 * bulk responses until its interrupt events are read.
 *
 * Build: cc -O2 -o om1-webcam om1-webcam.c -I/usr/pkg/include \
 *        -L/usr/pkg/lib -Wl,-R/usr/pkg/lib -lusb-1.0 -lpthread
 * Usage: om1-webcam [-d /dev/vcam] [-r WxH] [-v]
 */
#include <sys/ioctl.h>
#include <sys/time.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#include "vcamio.h"

#define VID 0x33a2
#define PID 0x0136
#define EP_OUT 0x05
#define EP_IN  0x86
#define EP_INT 0x83
#define TIMEOUT_MS 5000

#define PTP_CMD  1
#define PTP_DATA 2
#define PTP_RESP 3

#define OC_OpenSession        0x1002
#define OC_CloseSession       0x1003
#define OC_GetDeviceInfo      0x1001
#define OC_GetStorageIDs      0x1004
#define OC_GetObjectHandles   0x1007
#define OC_SetDevicePropValue 0x1016
#define OC_GetLiveView        0x9484
#define OC_ChangedProps       0x9486
#define OC_SetProperties      0x9489
#define OC_SetProperties2     0x948b

static const uint16_t props_9489[] = {
	0xD002,0xD003,0xD004,0xD005,0xD006,0xD007,0xD008,0xD009,
	0xD00C,0xD00D,0xD00E,0xD00F,0xD010,0xD011,0xD013,0xD014,
	0xD018,0xD01A,0xD01B,0xD01C,0xD01D,0xD01E,0xD01F,0xD020,
	0xD021,0xD022,0xD023,0xD024,0xD025,0xD026,0xD027,0xD028,
	0xD029,0xD02A,0xD02B,0xD02C,0xD02D,0xD02E,0xD02F,0xD030,
	0xD031,0xD032,0xD033,0xD034,0xD035,0xD036,0xD037,0xD038,
	0xD039,0xD03A,0xD03B,0xD03C,0xD03D,0xD03E,0xD03F,0xD040,
	0xD041,0xD042,0xD043,0xD044,0xD045,0xD046,0xD047,0xD048,
	0xD049,0xD04A,0xD04B,0xD04C,0xD04D,0xD04E,0xD04F,0xD050,
	0xD051,0xD052,0xD058,0xD059,0xD05F,0xD060,0xD061,0xD062,
	0xD064,0xD065,0xD066,0xD068,0xD069,0xD070,0xD073,0xD067,
	0xD05A,0xD05B,0xD063,0xD06A,0xD06B,0xD06C,0xD071,0xD072,
	0xD07A,0xD07B,0xD07C,0xD07D,0xD07F,0xD080,0xD081,0xD082,
	0xD086,0xD087,0xD08B,0xD08C,0xD08E,0xD08F,0xD097,0xD09F,
	0xD0C4,0xD0C5,0xD0A2,0xD0A3,0xD0A4,0xD0A6,0xD0A7,0xD0A8,
	0xD0A9,0xD0AA,0xD0AB,0xD0AC,0xD0AD,0xD0AE,0xD0B2,0xD0B3,
	0xD0B4,0xD0B5,0xD0B6,0xD0B7,0xD0B8,0xD0B9,0xD0BA,0xD0BC,
	0xD0BD,0xD0BE,0xD0BF,0xD0C0,0xD0C6,0xD0C7,0xD0C8,0xD0C9,
	0xD0CB,0xD0CC,0xD0CD,0xD0CE,0xD0CF,0xD0D0,0xD0D1,0xD0D2,
	0xD0D3,0xD0D4,0xD0D5,0xD0D6,0xD0D7,0xD0D8,0xD0D9,0xD0DA,
	0xD0DB,0xD0DC,0xD0DD,0xD0DE,0xD0E2,0xD0E3,0xD0E4,0xD0E5,
	0xD0E6,0xD0E7,0xD0E8,0xD0E9,0xD0EA,0xD0EC,0xD0EF,0xD0F0,
	0xD0F1,0xD0F2,0xD0F3,0xD0F4,0xD0F5,0xD0F6,0xD0F7,0xD0F8,
	0xD0F9,0xD0FA,0xD0FB,0xD0FC,0xD0FD,0xD0FE,0xD0FF,0xD100,
	0xD101,0xD102,0xD103,0xD104,0xD105,0xD106,0xD107,0xD108,
	0xD109,0xD10A,0xD10B,0xD10C,0xD10D,0xD10E,0xD10F,0xD110,
	0xD111,0xD112,0xD113,0xD114,0xD115,0xD116,0xD117,0xD118,
	0xD119,0xD11A,0xD11B,0xD11C,0xD11D,0xD11E,0xD11F,0xD120,
	0xD151,0xD152,0xD15A,0xD124,0xD125,0xD126,0xD127,0xD128,
	0xD12D,0xD12E,0xD12F,0xD130,0xD131,0xD134,0xD135,0xD136,
	0xD137,0xD138,0xD139,0xD13A,
};
static const uint16_t props_948b[] = {
	0xD053,0xD054,0xD055,0xD056,0xD057,0xD06D,0xD05C,0xD05D,
	0xD05E,0xD074,0xD075,0xD083,0xD084,0xD085,0xD0ED,0xD079,
	0xD0E1,
};

/* D06D live view sizes */
struct lvsize { const char *name; uint32_t code; uint32_t w, h; };
static const struct lvsize lvsizes[] = {
	{ "320x240",  0x014000F0,  320, 240 },
	{ "640x480",  0x028001E0,  640, 480 },
	{ "800x600",  0x03200258,  800, 600 },
	{ "1024x768", 0x04000300, 1024, 768 },
	{ "1280x960", 0x050003C0, 1280, 960 },
};

static libusb_device_handle *dev;
static uint32_t tid;
static volatile sig_atomic_t running = 1;
static int verbose;

static void onsig(int s) { (void)s; running = 0; }

static void *
drain_events(void *arg)
{
	unsigned char buf[64];
	int n;
	(void)arg;
	while (running)
		libusb_interrupt_transfer(dev, EP_INT, buf, sizeof(buf), &n, 200);
	return NULL;
}

static void
put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void
put16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t
get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t
get16(const unsigned char *p) { return p[0] | p[1] << 8; }

/*
 * One PTP transaction: command (+ data out), then data in (optional) and
 * the response.  Returns the response code or -1; *outlen gets the data.
 */
static int
ptp(uint16_t opcode, int nparams, const uint32_t *params,
    const unsigned char *sdata, int slen, unsigned char *rdata, int rmax, int *rlen)
{
	unsigned char pkt[64 + 1024], buf[64 * 1024];
	int len, n, rc, got = 0, attempts;

	tid++;
	len = 12 + nparams * 4;
	put32(pkt, len); put16(pkt + 4, PTP_CMD); put16(pkt + 6, opcode); put32(pkt + 8, tid);
	for (n = 0; n < nparams; n++)
		put32(pkt + 12 + n * 4, params[n]);
	rc = libusb_bulk_transfer(dev, EP_OUT, pkt, len, &n, TIMEOUT_MS);
	if (rc < 0) { warnx("command 0x%04x: %s", opcode, libusb_strerror(rc)); return -1; }
	if (sdata != NULL) {
		unsigned char *d = malloc(12 + slen);
		if (d == NULL) err(1, "malloc");
		put32(d, 12 + slen); put16(d + 4, PTP_DATA); put16(d + 6, opcode); put32(d + 8, tid);
		memcpy(d + 12, sdata, slen);
		rc = libusb_bulk_transfer(dev, EP_OUT, d, 12 + slen, &n, TIMEOUT_MS);
		free(d);
		if (rc < 0) { warnx("data 0x%04x: %s", opcode, libusb_strerror(rc)); return -1; }
	}
	if (rlen != NULL)
		*rlen = 0;
	for (attempts = 0; attempts < 10; attempts++) {
		rc = libusb_bulk_transfer(dev, EP_IN, buf, sizeof(buf), &n, TIMEOUT_MS);
		if (rc == LIBUSB_ERROR_TIMEOUT) { if (got) continue; warnx("0x%04x: timeout", opcode); return -1; }
		if (rc < 0) { warnx("read 0x%04x: %s", opcode, libusb_strerror(rc)); return -1; }
		if (n < 12)
			continue;
		uint32_t plen = get32(buf);
		uint16_t type = get16(buf + 4);
		if (type == PTP_DATA) {
			got = 1;
			int body = n - 12, remaining = (int)plen - n;
			if (rdata != NULL && body > 0) {
				int c = body < rmax ? body : rmax;
				memcpy(rdata, buf + 12, c); *rlen = c;
			}
			while (remaining > 0) {
				rc = libusb_bulk_transfer(dev, EP_IN, buf, sizeof(buf), &n, TIMEOUT_MS);
				if (rc < 0) break;
				if (rdata != NULL && *rlen < rmax) {
					int c = n < rmax - *rlen ? n : rmax - *rlen;
					memcpy(rdata + *rlen, buf, c); *rlen += c;
				}
				remaining -= n;
			}
		} else if (type == PTP_RESP) {
			return get16(buf + 6);
		}
	}
	warnx("0x%04x: no response", opcode);
	return -1;
}

static int
set_u16(uint16_t prop, uint16_t v)
{
	unsigned char d[2]; uint32_t p = prop;
	put16(d, v);
	return ptp(OC_SetDevicePropValue, 1, &p, d, 2, NULL, 0, NULL);
}

static int
set_u32(uint16_t prop, uint32_t v)
{
	unsigned char d[4]; uint32_t p = prop;
	put32(d, v);
	return ptp(OC_SetDevicePropValue, 1, &p, d, 4, NULL, 0, NULL);
}

static int
send_props(uint16_t opcode, const uint16_t *props, int count)
{
	unsigned char *d = malloc(4 + count * 2);
	int i, rc;
	if (d == NULL) err(1, "malloc");
	put32(d, count);
	for (i = 0; i < count; i++)
		put16(d + 4 + i * 2, props[i]);
	rc = ptp(opcode, 0, NULL, d, 4 + count * 2, NULL, 0, NULL);
	free(d);
	return rc;
}

static int
camera_init(const struct lvsize *lv)
{
	uint32_t one = 1, handles[3] = { 0xffffffff, 0, 0xffffffff };
	unsigned char info[4096]; int n;

	ptp(OC_CloseSession, 0, NULL, NULL, 0, NULL, 0, NULL);
	usleep(100000);
	if (ptp(OC_OpenSession, 1, &one, NULL, 0, NULL, 0, NULL) < 0) return -1;
	ptp(OC_GetDeviceInfo, 0, NULL, NULL, 0, info, sizeof(info), &n);
	ptp(OC_GetStorageIDs, 0, NULL, NULL, 0, info, sizeof(info), &n);
	ptp(OC_GetObjectHandles, 3, handles, NULL, 0, info, sizeof(info), &n);
	if (set_u16(0xD052, 1) < 0) return -1;		/* PC control mode */
	usleep(1000000);
	send_props(OC_SetProperties, props_9489, sizeof(props_9489) / 2);
	usleep(200000);
	send_props(OC_SetProperties2, props_948b, sizeof(props_948b) / 2);
	usleep(200000);
	set_u16(0xD078, 2);
	ptp(OC_ChangedProps, 0, NULL, NULL, 0, info, sizeof(info), &n);
	if (set_u32(0xD06D, lv->code) < 0) return -1;	/* live view on */
	usleep(1000000);
	if (verbose) warnx("camera initialised, live view %s", lv->name);
	return 0;
}

static void
camera_liveview(int on, const struct lvsize *lv)
{
	set_u32(0xD06D, on ? lv->code : 0);
	usleep(300000);
}

static void
camera_fini(const struct lvsize *lv)
{
	set_u32(0xD06D, 0); usleep(500000);
	set_u16(0xD052, 0); usleep(500000);
	ptp(OC_CloseSession, 0, NULL, NULL, 0, NULL, 0, NULL);
}

int
main(int argc, char **argv)
{
	const char *vcam = "/dev/vcam";
	const struct lvsize *lv = &lvsizes[3];
	struct vcam_format vf;
	pthread_t evt;
	unsigned char *frame;
	int ch, fd, rc, n, lvon = 0;
	unsigned frames = 0, dropped = 0;
	uint32_t p1 = 1;

	while ((ch = getopt(argc, argv, "d:r:v")) != -1) {
		switch (ch) {
		case 'd': vcam = optarg; break;
		case 'r': {
			size_t i; lv = NULL;
			for (i = 0; i < sizeof(lvsizes) / sizeof(lvsizes[0]); i++)
				if (strcmp(optarg, lvsizes[i].name) == 0) lv = &lvsizes[i];
			if (lv == NULL) errx(2, "sizes: 320x240 640x480 800x600 1024x768 1280x960");
			break; }
		case 'v': verbose = 1; break;
		default: fprintf(stderr, "usage: om1-webcam [-d /dev/vcam] [-r WxH] [-v]\n"); return 2;
		}
	}
	signal(SIGINT, onsig); signal(SIGTERM, onsig); signal(SIGHUP, onsig); signal(SIGPIPE, SIG_IGN);

	fd = open(vcam, O_WRONLY);
	if (fd < 0)
		err(1, "%s", vcam);
	vf.width = lv->w; vf.height = lv->h; vf.maxsize = 2 * 1024 * 1024;
	if (ioctl(fd, VCAMIOCSFORMAT, &vf) < 0)
		err(1, "VCAMIOCSFORMAT");

	if (libusb_init(NULL) < 0)
		errx(1, "libusb_init");
	dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
	if (dev == NULL)
		errx(1, "camera not found (USB %04x:%04x; Raw/Control mode?)", VID, PID);
	libusb_set_auto_detach_kernel_driver(dev, 1);
	rc = libusb_claim_interface(dev, 0);
	if (rc < 0) {
		int i;
		for (i = 0; i < 3; i++) libusb_detach_kernel_driver(dev, i);
		rc = libusb_claim_interface(dev, 0);
		if (rc < 0) errx(1, "claim interface: %s", libusb_strerror(rc));
	}
	pthread_create(&evt, NULL, drain_events, NULL);
	if (camera_init(lv) < 0)
		errx(1, "camera init failed");
	lvon = 1;
	frame = malloc(vf.maxsize);
	if (frame == NULL) err(1, "malloc");

	while (running) {
		struct vcam_state st;
		uint32_t ms = 1000;

		/* Nobody watching: live view off, sleep until a reader streams. */
		if (ioctl(fd, VCAMIOCGSTATE, &st) == 0 && !st.streaming) {
			if (lvon) { camera_liveview(0, lv); lvon = 0; if (verbose) warnx("no reader: live view off"); }
			ioctl(fd, VCAMIOCWAIT, &ms);
			continue;
		}
		if (!lvon) { camera_liveview(1, lv); lvon = 1; if (verbose) warnx("reader: live view on"); }
		rc = ptp(OC_GetLiveView, 1, &p1, NULL, 0, frame, vf.maxsize, &n);
		if (rc < 0) { if (!running) break; usleep(50000); continue; }
		if (n > 1024 && frame[0] == 0xff && frame[1] == 0xd8) {
			if (write(fd, frame, n) < 0) dropped++; else frames++;
			if (verbose && frames % 200 == 0) warnx("%u frames", frames);
		} else
			usleep(5000);
	}
	if (verbose) warnx("stopping: %u frames, %u write errors", frames, dropped);
	camera_fini(lv);
	running = 0;
	pthread_join(evt, NULL);
	libusb_release_interface(dev, 0);
	libusb_close(dev);
	libusb_exit(NULL);
	close(fd);
	return 0;
}
