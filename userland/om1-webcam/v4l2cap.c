/* v4l2cap DEV N: the mmap streaming path Firefox uses; dequeues N frames and reports sizes. */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/videoio.h>
#include <sys/poll.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
	const char *dev = argc > 1 ? argv[1] : "/dev/video4"; int want = argc > 2 ? atoi(argv[2]) : 5;
	int fd = open(dev, O_RDWR); if (fd < 0) err(1, "%s", dev);
	struct v4l2_capability cap; if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) err(1, "QUERYCAP");
	printf("card='%s' bus='%s' caps=0x%x\n", cap.card, cap.bus_info, cap.capabilities);
	struct v4l2_fmtdesc fd0; memset(&fd0, 0, sizeof fd0); fd0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (fd0.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fd0) == 0; fd0.index++) printf("fmt %u: %s 0x%08x flags 0x%x\n", fd0.index, fd0.description, fd0.pixelformat, fd0.flags);
	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt); fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) err(1, "G_FMT");
	printf("G_FMT %ux%u pixfmt 0x%08x sizeimage %u\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage);
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) warn("S_FMT");
	struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb); rb.count = 4; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) err(1, "REQBUFS"); printf("REQBUFS -> %u\n", rb.count);
	void *maps[8]; size_t lens[8]; unsigned i;
	for (i = 0; i < rb.count; i++) {
		struct v4l2_buffer b; memset(&b, 0, sizeof b); b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) err(1, "QUERYBUF");
		maps[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset); lens[i] = b.length;
		if (maps[i] == MAP_FAILED) err(1, "mmap");
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) err(1, "QBUF");
	}
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE; if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) err(1, "STREAMON");
	int got = 0; while (got < want) {
		struct pollfd p = { fd, POLLIN, 0 }; int r = poll(&p, 1, 5000);
		if (r <= 0) { printf("poll: %d (timeout?)\n", r); break; }
		struct v4l2_buffer b; memset(&b, 0, sizeof b); b.type = rb.type; b.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) { warn("DQBUF"); break; }
		unsigned char *d = maps[b.index];
		printf("frame %d: index %u bytesused %u seq %u first bytes %02x %02x\n", got, b.index, b.bytesused, b.sequence, d[0], d[1]);
		got++; if (ioctl(fd, VIDIOC_QBUF, &b) < 0) err(1, "QBUF");
	}
	ioctl(fd, VIDIOC_STREAMOFF, &type); return got == want ? 0 : 1;
}
