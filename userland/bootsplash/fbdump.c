/* fbdump [tty] > file: raw copy of the console framebuffer (for checking the
 * splash from afar).  Prints "WxHxdepth stride" on stderr. */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dev/wscons/wsconsio.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) {
	const char *tty = argc > 1 ? argv[1] : "/dev/ttyE0";
	struct wsdisplay_fbinfo info; u_int stride; int fd = open(tty, O_RDWR);
	if (fd < 0) err(1, "%s", tty);
	if (ioctl(fd, WSDISPLAYIO_GINFO, &info) < 0 || ioctl(fd, WSDISPLAYIO_LINEBYTES, &stride) < 0) err(1, "ioctl");
	size_t size = (size_t)stride * info.height;
	void *fb = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) err(1, "mmap");
	fprintf(stderr, "%ux%ux%u stride %u\n", info.width, info.height, info.depth, stride);
	fwrite(fb, 1, size, stdout); return 0;
}
