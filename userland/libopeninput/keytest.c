/* keytest: feed scripted wscons events through the installed libopeninput
   and check that per-key press counts (what wlroots' keyboard group keeps)
   always return to zero. */
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <dev/wscons/wsconsio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libinput.h>

static int pipefd[2];

/* Interpose ioctl: claim the fake device is a USB keyboard. */
int
ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd == pipefd[0] && request == WSKBDIO_GTYPE) {
		*(int *)arg = WSKBD_TYPE_USB;
		return 0;
	}
	if (fd == pipefd[0]) {
		errno = ENOTTY;
		return -1;
	}
	return (int)__syscall(SYS_ioctl, fd, request, arg);
}

static int
open_restricted(const char *path, int flags, void *data)
{
	(void)flags; (void)data;
	return strncmp(path, "/dev/wskbd", 10) == 0 ? pipefd[0] : -ENOENT;
}

static void
close_restricted(int fd, void *data)
{
	(void)fd; (void)data;
}

static const struct libinput_interface iface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

static int count[1024];

static void
send(struct libinput *li, unsigned type, int value, const char *what)
{
	struct wscons_event ev;
	struct libinput_event *e;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.value = value;
	if (write(pipefd[1], &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
	libinput_dispatch(li);
	printf("%-26s ->", what);
	while ((e = libinput_get_event(li)) != NULL) {
		if (libinput_event_get_type(e) == LIBINPUT_EVENT_KEYBOARD_KEY) {
			struct libinput_event_keyboard *k =
			    libinput_event_get_keyboard_event(e);
			unsigned key = libinput_event_keyboard_get_key(k);
			int pressed = libinput_event_keyboard_get_key_state(k) ==
			    LIBINPUT_KEY_STATE_PRESSED;
			count[key] += pressed ? 1 : -1;
			printf(" key%u %s", key, pressed ? "down" : "up");
		}
		libinput_event_destroy(e);
	}
	printf("\n");
}

int
main(void)
{
	enum { CTRL = 0xe0, SHIFT = 0xe1, A = 0x04 };	/* USB HID usages */
	struct libinput *li;
	int bad = 0, i;

	if (pipe(pipefd) == -1)
		return 1;
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	li = libinput_path_create_context(&iface, NULL);
	if (li == NULL || libinput_path_add_device(li, "/dev/wskbd9") == NULL) {
		fprintf(stderr, "cannot add fake keyboard\n");
		return 1;
	}
	libinput_dispatch(li);
	while (libinput_get_event(li) != NULL)
		;
	send(li, WSCONS_EVENT_KEY_DOWN, CTRL, "ctrl down");
	send(li, WSCONS_EVENT_KEY_DOWN, A, "a down");
	send(li, WSCONS_EVENT_KEY_UP, A, "a up");
	send(li, WSCONS_EVENT_KEY_DOWN, CTRL, "ctrl down (duplicate)");
	send(li, WSCONS_EVENT_KEY_UP, CTRL, "ctrl up");
	send(li, WSCONS_EVENT_KEY_DOWN, CTRL, "ctrl down again");
	send(li, WSCONS_EVENT_KEY_UP, CTRL, "ctrl up again");
	send(li, WSCONS_EVENT_KEY_UP, SHIFT, "shift up (never down)");
	send(li, WSCONS_EVENT_KEY_DOWN, CTRL, "ctrl down");
	send(li, WSCONS_EVENT_ALL_KEYS_UP, 0, "all keys up");
	send(li, WSCONS_EVENT_KEY_UP, CTRL, "ctrl up (after all up)");
	for (i = 0; i < 1024; i++)
		if (count[i] != 0) {
			printf("FAIL: key%d press count %d\n", i, count[i]);
			bad = 1;
		}
	printf(bad ? "RESULT: keys left held\n" : "RESULT: all press counts back to 0\n");
	libinput_unref(li);
	return bad;
}
