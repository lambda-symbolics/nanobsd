/* keylog NAME: a toplevel that prints every wl_keyboard event it gets. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp; static struct wl_shm *shm;
static struct xdg_wm_base *wm; static struct wl_seat *seat;
static struct wl_surface *surf; static struct xdg_surface *xs;
static const char *name; static int W = 200, H = 200;

static void attach(void) {
	char path[] = "/tmp/keylogXXXXXX"; int fd = mkstemp(path); unlink(path);
	ftruncate(fd, W * H * 4);
	void *p = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset(p, 0x80, W * H * 4);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, W * H * 4);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(surf, b, 0, 0); wl_surface_damage(surf, 0, 0, W, H); wl_surface_commit(surf);
}
static void ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wml = { ping };
static void xs_conf(void *d, struct xdg_surface *x, uint32_t s) { xdg_surface_ack_configure(x, s); attach(); }
static const struct xdg_surface_listener xsl = { xs_conf };
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { if (w > 0) W = w; if (h > 0) H = h; }
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tll = { tl_conf, tl_close };

static void km(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { close(fd); }
static void ken(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {
	uint32_t *kc; printf("%s enter keys:", name);
	wl_array_for_each(kc, keys) printf(" %u", *kc);
	printf("\n"); fflush(stdout);
}
static void kle(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { printf("%s leave\n", name); fflush(stdout); }
static void kk(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
	printf("%s key %u %s\n", name, key, st ? "pressed" : "released"); fflush(stdout);
}
static void kmod(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
	printf("%s mods %u\n", name, a); fflush(stdout);
}
static void krep(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {}
static const struct wl_keyboard_listener kl = { km, ken, kle, kk, kmod, krep };
static void caps(void *d, struct wl_seat *s, uint32_t c) {
	static struct wl_keyboard *kb;
	if ((c & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) { kb = wl_seat_get_keyboard(s); wl_keyboard_add_listener(kb, &kl, NULL); }
}
static void sname(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener sl = { caps, sname };
static void glob(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v) {
	if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
	else if (!strcmp(i, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
	else if (!strcmp(i, "xdg_wm_base")) { wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wml, NULL); }
	else if (!strcmp(i, "wl_seat")) { seat = wl_registry_bind(r, id, &wl_seat_interface, 5); wl_seat_add_listener(seat, &sl, NULL); }
}
static void gr(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener rl = { glob, gr };

int main(int argc, char **argv) {
	name = argc > 1 ? argv[1] : "keylog";
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { perror("connect"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &rl, NULL); wl_display_roundtrip(dpy); wl_display_roundtrip(dpy);
	surf = wl_compositor_create_surface(comp);
	xs = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xs, &xsl, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tll, NULL); xdg_toplevel_set_app_id(tl, name); xdg_toplevel_set_title(tl, name);
	wl_surface_commit(surf);
	while (wl_display_dispatch(dpy) != -1) ;
	return 0;
}
