/*
 * freezeshot-core: the Wayland half of freezeshot (the Lisp half decides
 * what the input means).  Captures every output with wlr-screencopy, shows
 * the captured frames on overlay layer surfaces so the screen holds still,
 * hands pointer and key events to the caller one at a time, draws the
 * selection, and writes a crop of the frozen frame as PNG.
 *
 * Power: nothing is drawn per frame.  The overlay is two shm copies of the
 * capture; moving the selection rewrites only the old and new border strips
 * of the buffer the compositor has released, damages just those strips and
 * waits for the frame callback before drawing again.
 */
#define _NETBSD_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cairo.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define FZ_MAX_OUTPUTS	8
#define FZ_QUEUE	64
#define FZ_BORDER	2		/* selection line width, buffer pixels */
#define FZ_DASH		6		/* dash and gap length */
#define FZ_ACCENT	0xffe46876u	/* status bar accent, XRGB8888 */
#define FZ_IDLE_SEC	60		/* no input this long: give the screen back */

enum { FZ_EV_MOTION = 1, FZ_EV_BUTTON = 2, FZ_EV_KEY = 3 };

/* Mirrored by the Lisp side (freezeshot.lisp, fz-event). */
struct fz_event {
	int32_t type;
	int32_t output;		/* index of the output the pointer is on */
	double x, y;		/* buffer pixels on that output */
	uint32_t code;		/* evdev button or key code */
	int32_t pressed;
};

struct fz_rect {
	int x, y, w, h;
	int set;
};

struct fz_buf {
	struct wl_buffer *buffer;
	uint32_t *data;
	size_t size;
	int busy;
	struct fz_rect drawn;	/* selection currently drawn into data */
};

struct fz;

struct fz_output {
	struct fz *fz;
	struct wl_output *wl_output;
	int32_t scale;

	/* capture */
	struct zwlr_screencopy_frame_v1 *frame;
	uint32_t format, width, height, stride;
	int y_invert, buffer_known, done, failed;
	struct wl_buffer *cap_buffer;
	void *cap_data;
	size_t cap_size;
	uint32_t *pristine;	/* XRGB8888, top row first */

	/* overlay */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	int configured, shown;
	struct fz_buf bufs[2];
	struct wl_callback *frame_cb;
	struct fz_rect want;	/* selection the caller asked for */
	int dirty;
};

struct fz {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_screencopy_manager_v1 *screencopy;
	struct wp_cursor_shape_manager_v1 *cursor_shape;
	struct wp_cursor_shape_device_v1 *cursor_device;

	struct fz_output outputs[FZ_MAX_OUTPUTS];
	int noutputs;
	int pointer_output;
	double px, py;

	struct fz_event queue[FZ_QUEUE];
	int qhead, qlen;
	char error[256];
};

static void
fz_fail(struct fz *fz, const char *msg)
{
	if (fz->error[0] == '\0')
		snprintf(fz->error, sizeof(fz->error), "%s", msg);
}

static int fz_debug;

static void
fz_push(struct fz *fz, const struct fz_event *ev)
{
	if (fz_debug)
		fprintf(stderr, "freezeshot: event %d output %d %.1f,%.1f code %#x %s\n",
		    ev->type, ev->output, ev->x, ev->y, ev->code,
		    ev->pressed ? "down" : "up");
	if (fz->qlen == FZ_QUEUE) {
		/* Full: the caller is slow; keep the newest motion instead. */
		if (ev->type != FZ_EV_MOTION)
			fz->qlen--;
		else
			return;
	}
	fz->queue[(fz->qhead + fz->qlen) % FZ_QUEUE] = *ev;
	fz->qlen++;
}

/* ---- shm ------------------------------------------------------------- */

static struct wl_buffer *
fz_shm_buffer(struct fz *fz, uint32_t format, uint32_t width, uint32_t height,
    uint32_t stride, void **data, size_t *size)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	size_t len = (size_t)stride * height;
	void *map;
	int fd;

	fd = memfd_create("freezeshot", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)len) < 0) {
		close(fd);
		return NULL;
	}
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	pool = wl_shm_create_pool(fz->shm, fd, (int32_t)len);
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width,
	    (int32_t)height, (int32_t)stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	*data = map;
	*size = len;
	return buffer;
}

/* ---- outputs ---------------------------------------------------------- */

static void
output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
    int32_t pw, int32_t ph, int32_t subpixel, const char *make,
    const char *model, int32_t transform)
{
}

static void
output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
    int32_t h, int32_t refresh)
{
}

static void
output_done(void *data, struct wl_output *o)
{
}

static void
output_scale(void *data, struct wl_output *o, int32_t factor)
{
	struct fz_output *out = data;

	out->scale = factor > 0 ? factor : 1;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

/* ---- capture ---------------------------------------------------------- */

static void
frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride)
{
	struct fz_output *out = data;

	out->format = format;
	out->width = width;
	out->height = height;
	out->stride = stride;
	out->buffer_known = 1;
}

static void
frame_copy(struct fz_output *out)
{
	out->cap_buffer = fz_shm_buffer(out->fz, out->format, out->width,
	    out->height, out->stride, &out->cap_data, &out->cap_size);
	if (out->cap_buffer == NULL) {
		out->failed = 1;
		fz_fail(out->fz, "cannot allocate the capture buffer");
		return;
	}
	zwlr_screencopy_frame_v1_copy(out->frame, out->cap_buffer);
}

static void
frame_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
	struct fz_output *out = data;

	out->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

/* Convert the capture to XRGB8888, top row first. */
static int
frame_convert(struct fz_output *out)
{
	uint32_t x, y;
	int swap;

	switch (out->format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
		swap = 0;
		break;
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_XBGR8888:
		swap = 1;
		break;
	default:
		fz_fail(out->fz, "unsupported capture format");
		return -1;
	}
	out->pristine = malloc((size_t)out->width * out->height * 4);
	if (out->pristine == NULL)
		return -1;
	for (y = 0; y < out->height; y++) {
		uint32_t sy = out->y_invert ? out->height - 1 - y : y;
		const uint32_t *src = (const uint32_t *)
		    ((const uint8_t *)out->cap_data + (size_t)sy * out->stride);
		uint32_t *dst = out->pristine + (size_t)y * out->width;

		for (x = 0; x < out->width; x++) {
			uint32_t p = src[x];

			if (swap)
				p = (p & 0x0000ff00u) | ((p & 0xffu) << 16) |
				    ((p >> 16) & 0xffu);
			dst[x] = p | 0xff000000u;
		}
	}
	return 0;
}

static void
frame_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t sec_hi,
    uint32_t sec_lo, uint32_t nsec)
{
	struct fz_output *out = data;

	if (frame_convert(out) < 0)
		out->failed = 1;
	munmap(out->cap_data, out->cap_size);
	wl_buffer_destroy(out->cap_buffer);
	out->cap_buffer = NULL;
	zwlr_screencopy_frame_v1_destroy(out->frame);
	out->frame = NULL;
	out->done = 1;
}

static void
frame_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
	struct fz_output *out = data;

	fz_fail(out->fz, "the compositor refused the capture");
	out->failed = 1;
	out->done = 1;
}

static void
frame_damage(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t x,
    uint32_t y, uint32_t w, uint32_t h)
{
}

static void
frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
    uint32_t format, uint32_t width, uint32_t height)
{
}

static void
frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
	struct fz_output *out = data;

	if (out->buffer_known)
		frame_copy(out);
	else {
		fz_fail(out->fz, "no shm buffer offered for the capture");
		out->failed = 1;
		out->done = 1;
	}
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

/* ---- drawing ---------------------------------------------------------- */

static void
buf_restore(struct fz_output *out, struct fz_buf *b, int x, int y, int w,
    int h)
{
	int r;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)out->width)
		w = (int)out->width - x;
	if (y + h > (int)out->height)
		h = (int)out->height - y;
	if (w <= 0 || h <= 0)
		return;
	for (r = y; r < y + h; r++)
		memcpy(b->data + (size_t)r * out->width + x,
		    out->pristine + (size_t)r * out->width + x, (size_t)w * 4);
	wl_surface_damage_buffer(out->surface, x, y, w, h);
}

static void
buf_line(struct fz_output *out, struct fz_buf *b, int x, int y, int w, int h)
{
	int r, c;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)out->width)
		w = (int)out->width - x;
	if (y + h > (int)out->height)
		h = (int)out->height - y;
	if (w <= 0 || h <= 0)
		return;
	for (r = y; r < y + h; r++) {
		uint32_t *row = b->data + (size_t)r * out->width;

		for (c = x; c < x + w; c++)
			if (((r + c) / FZ_DASH) % 2 == 0)
				row[c] = FZ_ACCENT;
	}
	wl_surface_damage_buffer(out->surface, x, y, w, h);
}

/* The four border strips of R (FZ_BORDER thick, inside R). */
#define FZ_STRIPS(R, F) do {						\
	F((R).x, (R).y, (R).w, FZ_BORDER);				\
	F((R).x, (R).y + (R).h - FZ_BORDER, (R).w, FZ_BORDER);		\
	F((R).x, (R).y, FZ_BORDER, (R).h);				\
	F((R).x + (R).w - FZ_BORDER, (R).y, FZ_BORDER, (R).h);		\
} while (0)

static void frame_done(void *, struct wl_callback *, uint32_t);
static const struct wl_callback_listener frame_cb_listener = {
	.done = frame_done,
};

static void
out_draw(struct fz_output *out)
{
	struct fz_buf *b = NULL;
	int i;

	if (!out->shown || !out->dirty || out->frame_cb != NULL)
		return;
	for (i = 0; i < 2; i++)
		if (!out->bufs[i].busy) {
			b = &out->bufs[i];
			break;
		}
	if (b == NULL)
		return;		/* both held by the compositor: retry on release */

#define RESTORE(x, y, w, h) buf_restore(out, b, x, y, w, h)
#define LINE(x, y, w, h) buf_line(out, b, x, y, w, h)
	if (b->drawn.set)
		FZ_STRIPS(b->drawn, RESTORE);
	if (out->want.set && out->want.w > 0 && out->want.h > 0)
		FZ_STRIPS(out->want, LINE);
#undef RESTORE
#undef LINE
	b->drawn = out->want;
	out->dirty = 0;

	wl_surface_attach(out->surface, b->buffer, 0, 0);
	b->busy = 1;
	out->frame_cb = wl_surface_frame(out->surface);
	wl_callback_add_listener(out->frame_cb, &frame_cb_listener, out);
	wl_surface_commit(out->surface);
}

static void
frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct fz_output *out = data;

	wl_callback_destroy(cb);
	out->frame_cb = NULL;
	out_draw(out);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct fz_output *out = data;
	int i;

	for (i = 0; i < 2; i++)
		if (out->bufs[i].buffer == buffer)
			out->bufs[i].busy = 0;
	out_draw(out);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

/* ---- overlay ---------------------------------------------------------- */

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial,
    uint32_t w, uint32_t h)
{
	struct fz_output *out = data;

	zwlr_layer_surface_v1_ack_configure(l, serial);
	if (out->configured)
		return;
	out->configured = 1;
	wl_surface_set_buffer_scale(out->surface, out->scale);
	wl_surface_attach(out->surface, out->bufs[0].buffer, 0, 0);
	out->bufs[0].busy = 1;
	wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(out->surface);
	out->shown = 1;
}

static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
	struct fz_output *out = data;

	fz_fail(out->fz, "overlay closed by the compositor");
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* ---- input ------------------------------------------------------------ */

static int
fz_output_of(struct fz *fz, struct wl_surface *surface)
{
	int i;

	for (i = 0; i < fz->noutputs; i++)
		if (fz->outputs[i].surface == surface)
			return i;
	return -1;
}

static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	struct fz *fz = data;
	struct fz_event ev = { .type = FZ_EV_MOTION };
	int i = fz_output_of(fz, surface);

	if (fz_debug)
		fprintf(stderr, "freezeshot: pointer enter output %d\n", i);
	if (fz->cursor_device != NULL)
		wp_cursor_shape_device_v1_set_shape(fz->cursor_device, serial,
		    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
	if (i < 0)
		return;
	fz->pointer_output = i;
	fz->px = wl_fixed_to_double(sx) * fz->outputs[i].scale;
	fz->py = wl_fixed_to_double(sy) * fz->outputs[i].scale;
	ev.output = i;
	ev.x = fz->px;
	ev.y = fz->py;
	fz_push(fz, &ev);
}

static void
pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surface)
{
}

static void
pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
	struct fz *fz = data;
	struct fz_event ev = { .type = FZ_EV_MOTION };
	int i = fz->pointer_output;

	if (i < 0)
		return;
	fz->px = wl_fixed_to_double(sx) * fz->outputs[i].scale;
	fz->py = wl_fixed_to_double(sy) * fz->outputs[i].scale;
	ev.output = i;
	ev.x = fz->px;
	ev.y = fz->py;
	fz_push(fz, &ev);
}

static void
pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
	struct fz *fz = data;
	struct fz_event ev = { .type = FZ_EV_BUTTON };

	ev.output = fz->pointer_output;
	ev.x = fz->px;
	ev.y = fz->py;
	ev.code = button;
	ev.pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	fz_push(fz, &ev);
}

static void
pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
    wl_fixed_t value)
{
}

static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p,
    uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t t,
    uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
    uint32_t axis, int32_t discrete) {}
static void pointer_axis_value120(void *data, struct wl_pointer *p,
    uint32_t axis, int32_t v) {}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p,
    uint32_t axis, uint32_t dir) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

static void
keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int fd,
    uint32_t size)
{
	close(fd);	/* raw evdev codes are enough: Escape and Return */
}

static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s) {}

static void
keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state)
{
	struct fz *fz = data;
	struct fz_event ev = { .type = FZ_EV_KEY };

	ev.output = fz->pointer_output;
	ev.x = fz->px;
	ev.y = fz->py;
	ev.code = key;
	ev.pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	fz_push(fz, &ev);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k,
    uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock,
    uint32_t group) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *k,
    int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct fz *fz = data;

	if (fz_debug)
		fprintf(stderr, "freezeshot: seat capabilities %#x\n", caps);
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && fz->pointer == NULL) {
		fz->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(fz->pointer, &pointer_listener, fz);
		if (fz->cursor_shape != NULL)
			fz->cursor_device = wp_cursor_shape_manager_v1_get_pointer(
			    fz->cursor_shape, fz->pointer);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && fz->keyboard == NULL) {
		fz->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(fz->keyboard, &keyboard_listener, fz);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- registry --------------------------------------------------------- */

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void
registry_global(void *data, struct wl_registry *r, uint32_t name,
    const char *iface, uint32_t version)
{
	struct fz *fz = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0)
		fz->compositor = wl_registry_bind(r, name,
		    &wl_compositor_interface, MIN(version, 4));
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		fz->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0 && fz->seat == NULL) {
		/* Listen at once: capabilities is sent right after the bind. */
		fz->seat = wl_registry_bind(r, name, &wl_seat_interface,
		    MIN(version, 5));
		wl_seat_add_listener(fz->seat, &seat_listener, fz);
	}
	else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
		fz->layer_shell = wl_registry_bind(r, name,
		    &zwlr_layer_shell_v1_interface, MIN(version, 3));
	else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0)
		fz->screencopy = wl_registry_bind(r, name,
		    &zwlr_screencopy_manager_v1_interface, MIN(version, 3));
	else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0)
		fz->cursor_shape = wl_registry_bind(r, name,
		    &wp_cursor_shape_manager_v1_interface, 1);
	else if (strcmp(iface, wl_output_interface.name) == 0 &&
	    fz->noutputs < FZ_MAX_OUTPUTS) {
		struct fz_output *out = &fz->outputs[fz->noutputs++];

		out->fz = fz;
		out->scale = 1;
		out->wl_output = wl_registry_bind(r, name, &wl_output_interface,
		    MIN(version, 2));
		wl_output_add_listener(out->wl_output, &output_listener, out);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- API (called from Lisp) ------------------------------------------- */

const char *
fz_error(struct fz *fz)
{
	return fz->error[0] ? fz->error : "unknown error";
}

/* Connect and capture every output.  NULL only if out of memory; check
 * fz_error() when fz_noutputs() is 0. */
struct fz *
fz_open(void)
{
	struct fz *fz;
	int i, pending;

	if ((fz = calloc(1, sizeof(*fz))) == NULL)
		return NULL;
	fz_debug = getenv("FREEZESHOT_DEBUG") != NULL;
	fz->pointer_output = -1;
	if ((fz->display = wl_display_connect(NULL)) == NULL) {
		fz_fail(fz, "cannot connect to the Wayland display");
		return fz;
	}
	fz->registry = wl_display_get_registry(fz->display);
	wl_registry_add_listener(fz->registry, &registry_listener, fz);
	wl_display_roundtrip(fz->display);
	if (fz->compositor == NULL || fz->shm == NULL ||
	    fz->layer_shell == NULL || fz->screencopy == NULL) {
		fz_fail(fz, "compositor lacks layer-shell or screencopy");
		fz->noutputs = 0;
		return fz;
	}
	wl_display_roundtrip(fz->display);	/* output scales */

	/* Capture first, as early as possible: this is the frozen moment. */
	for (i = 0; i < fz->noutputs; i++) {
		struct fz_output *out = &fz->outputs[i];

		out->frame = zwlr_screencopy_manager_v1_capture_output(
		    fz->screencopy, 0, out->wl_output);
		zwlr_screencopy_frame_v1_add_listener(out->frame,
		    &frame_listener, out);
	}
	do {
		pending = 0;
		for (i = 0; i < fz->noutputs; i++)
			pending |= !fz->outputs[i].done;
		if (pending && wl_display_dispatch(fz->display) < 0) {
			fz_fail(fz, "display connection lost while capturing");
			fz->noutputs = 0;
			return fz;
		}
	} while (pending);
	for (i = 0; i < fz->noutputs; i++)
		if (fz->outputs[i].failed) {
			fz->noutputs = 0;
			return fz;
		}

	return fz;
}

int
fz_noutputs(struct fz *fz)
{
	return fz->noutputs;
}

int
fz_output_width(struct fz *fz, int i)
{
	return (int)fz->outputs[i].width;
}

int
fz_output_height(struct fz *fz, int i)
{
	return (int)fz->outputs[i].height;
}

/* Put the frozen frames over every output.  0 on success. */
int
fz_show(struct fz *fz)
{
	int i, j, waiting;

	for (i = 0; i < fz->noutputs; i++) {
		struct fz_output *out = &fz->outputs[i];

		for (j = 0; j < 2; j++) {
			struct fz_buf *b = &out->bufs[j];
			void *map;

			b->buffer = fz_shm_buffer(fz, WL_SHM_FORMAT_XRGB8888,
			    out->width, out->height, out->width * 4, &map,
			    &b->size);
			if (b->buffer == NULL) {
				fz_fail(fz, "cannot allocate the overlay");
				return -1;
			}
			b->data = map;
			memcpy(b->data, out->pristine, b->size);
			wl_buffer_add_listener(b->buffer, &buffer_listener, out);
		}
		out->surface = wl_compositor_create_surface(fz->compositor);
		out->layer = zwlr_layer_shell_v1_get_layer_surface(
		    fz->layer_shell, out->surface, out->wl_output,
		    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "freezeshot");
		zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener,
		    out);
		zwlr_layer_surface_v1_set_anchor(out->layer,
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(out->layer, 0, 0);
		zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
		zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer,
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		wl_surface_commit(out->surface);
	}
	do {
		waiting = 0;
		for (i = 0; i < fz->noutputs; i++)
			waiting |= !fz->outputs[i].shown;
		if (waiting && wl_display_dispatch(fz->display) < 0) {
			fz_fail(fz, "display connection lost");
			return -1;
		}
	} while (waiting && fz->error[0] == '\0');
	return fz->error[0] ? -1 : 0;
}

/* Block until an input event; 0 and *ev filled, -1 on error or after
 * FZ_IDLE_SEC without input (the overlay holds the keyboard exclusively, so
 * it must never stay up by itself). */
int
fz_next_event(struct fz *fz, struct fz_event *ev)
{
	const struct timespec idle = { .tv_sec = FZ_IDLE_SEC };
	int n;

	while (fz->qlen == 0) {
		if (fz->error[0] != '\0')
			return -1;
		n = wl_display_dispatch_timeout(fz->display, &idle);
		if (n < 0) {
			fz_fail(fz, "display connection lost");
			return -1;
		}
		if (n == 0 && fz->qlen == 0) {
			fz_fail(fz, "no input, giving up");
			return -1;
		}
	}
	*ev = fz->queue[fz->qhead];
	fz->qhead = (fz->qhead + 1) % FZ_QUEUE;
	fz->qlen--;
	return 0;
}

/* Show selection (x, y, w, h) on output i; w or h <= 0 clears it.  Other
 * outputs lose theirs. */
void
fz_select(struct fz *fz, int i, int x, int y, int w, int h)
{
	int j;

	for (j = 0; j < fz->noutputs; j++) {
		struct fz_output *out = &fz->outputs[j];
		struct fz_rect r = { 0 };

		if (j == i && w > 0 && h > 0) {
			r.x = x;
			r.y = y;
			r.w = w;
			r.h = h;
			r.set = 1;
		}
		if (memcmp(&r, &out->want, sizeof(r)) == 0)
			continue;
		out->want = r;
		out->dirty = 1;
		out_draw(out);
	}
	wl_display_flush(fz->display);
}

/* Take the overlays down (the screen moves again). */
void
fz_hide(struct fz *fz)
{
	int i;

	for (i = 0; i < fz->noutputs; i++) {
		struct fz_output *out = &fz->outputs[i];

		if (out->layer != NULL) {
			zwlr_layer_surface_v1_destroy(out->layer);
			out->layer = NULL;
		}
		if (out->surface != NULL) {
			wl_surface_destroy(out->surface);
			out->surface = NULL;
		}
		out->shown = 0;
	}
	if (fz->display != NULL)
		wl_display_roundtrip(fz->display);
}

/* Write (x, y, w, h) of output i's frozen frame to PATH as PNG. */
int
fz_save_png(struct fz *fz, int i, int x, int y, int w, int h,
    const char *path)
{
	struct fz_output *out;
	cairo_surface_t *src, *dst;
	cairo_t *cr;
	cairo_status_t st;

	if (i < 0 || i >= fz->noutputs || w <= 0 || h <= 0) {
		fz_fail(fz, "empty selection");
		return -1;
	}
	out = &fz->outputs[i];
	src = cairo_image_surface_create_for_data((unsigned char *)out->pristine,
	    CAIRO_FORMAT_RGB24, (int)out->width, (int)out->height,
	    (int)out->width * 4);
	dst = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cr = cairo_create(dst);
	cairo_set_source_surface(cr, src, -x, -y);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	st = cairo_surface_write_to_png(dst, path);
	cairo_surface_destroy(dst);
	cairo_surface_destroy(src);
	if (st != CAIRO_STATUS_SUCCESS) {
		fz_fail(fz, cairo_status_to_string(st));
		return -1;
	}
	return 0;
}

void
fz_close(struct fz *fz)
{
	if (fz->display != NULL)
		wl_display_disconnect(fz->display);
	/* The process exits right after: let it reclaim the rest. */
}
