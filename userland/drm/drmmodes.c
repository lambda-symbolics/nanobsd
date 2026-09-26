/* drmmodes: print every connector's modes with full timings (read-only). */
#include <fcntl.h>
#include <stdio.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void) {
	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { perror("card0"); return 1; }
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { perror("resources"); return 1; }
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c || c->connection != DRM_MODE_CONNECTED) continue;
		printf("connector %u type %u: %d modes\n", c->connector_id, c->connector_type, c->count_modes);
		for (int m = 0; m < c->count_modes; m++) {
			drmModeModeInfo *x = &c->modes[m];
			printf("  %s clock %u kHz  h %u %u %u %u  v %u %u %u %u  %u Hz flags 0x%x type 0x%x\n",
			    x->name, x->clock, x->hdisplay, x->hsync_start, x->hsync_end, x->htotal,
			    x->vdisplay, x->vsync_start, x->vsync_end, x->vtotal, x->vrefresh, x->flags, x->type);
		}
	}
	return 0;
}
