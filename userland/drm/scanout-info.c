/*
 * scanout-info: list the framebuffers the KMS planes are scanning out
 * (size, pitch, i915 tiling), so a capture tool knows what it is reading.
 *
 * cc -O2 -Wall -I/usr/X11R7/include/libdrm -o scanout-info scanout-info.c \
 *    -L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib -ldrm
 */
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <i915_drm.h>

int
main(void)
{
	drmModePlaneResPtr res;
	uint32_t i;
	int fd;

	if ((fd = open("/dev/dri/card0", O_RDWR)) == -1)
		err(1, "card0");
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if ((res = drmModeGetPlaneResources(fd)) == NULL)
		err(1, "planes");
	for (i = 0; i < res->count_planes; i++) {
		drmModePlanePtr p = drmModeGetPlane(fd, res->planes[i]);
		drmModeFBPtr fb;
		struct drm_i915_gem_get_tiling t;

		if (p == NULL || p->fb_id == 0)
			continue;
		/* NetBSD's DRM (Linux 5.6) has no GETFB2: no modifier, ask
		 * i915 for the legacy tiling instead. */
		fb = drmModeGetFB(fd, p->fb_id);
		if (fb == NULL) {
			printf("plane %u fb %u: GetFB failed\n", p->plane_id,
			    p->fb_id);
			continue;
		}
		memset(&t, 0, sizeof(t));
		t.handle = fb->handle;
		ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING, &t);
		printf("plane %u crtc %u fb %u: %ux%u bpp %u pitch %u handle %u "
		    "tiling %u swizzle %u\n", p->plane_id, p->crtc_id,
		    fb->fb_id, fb->width, fb->height, fb->bpp, fb->pitch,
		    fb->handle, t.tiling_mode, t.swizzle_mode);
	}
	return 0;
}
