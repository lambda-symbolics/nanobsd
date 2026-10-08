#!/bin/sh
# Rebuild pkgsrc multimedia/libva with its Wayland backend (libva-wayland).
# pkgsrc hard-codes --disable-wayland; mpv's zero-copy hwdec=vaapi needs
# vaGetDisplayWl to get a VA display inside a Wayland window (its other path,
# vaGetDisplayDRM, wants a drm_params_v2 resource the Wayland EGL context
# does not provide).  See docs/mpv-zero-copy.org.
# Never run pkgin or pkg_add while this runs.  Rebuild mpv afterwards
# (userland/mpv/build.sh with REPLACE=1) so meson picks up vaapi-wayland.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd /usr/pkgsrc/multimedia/libva
if ! grep -q 'enable-wayland' Makefile; then
	cp Makefile Makefile.orig-lispbsd
	sed -i.bak -e '/^# Might be useful to have this but/,/^CONFIGURE_ARGS+=	--disable-wayland$/d' Makefile
	awk '/^CONFIGURE_ARGS\+=	--disable-glx/ {
		print "# LISPBSD: the Wayland backend (vaGetDisplayWl) lets a VA-API client in a"
		print "# Wayland window get a VA display without X11, which mpv needs for zero-copy"
		print "# hwdec=vaapi.  libva finds the GPU through wl_drm or linux-dmabuf feedback."
		print "CONFIGURE_ARGS+=	--enable-wayland" }
		{ print }
		/^\.include "\.\.\/\.\.\/x11\/libX11\/buildlink3\.mk"/ {
		print ".include \"../../devel/wayland/buildlink3.mk\"" }' Makefile > Makefile.new
	mv Makefile.new Makefile
	rm -f Makefile.bak
fi
# PLIST: libtool adds the .so entries itself, the rest must be listed.
for e in include/va/va_backend_wayland.h include/va/va_wayland.h lib/libva-wayland.la lib/pkgconfig/libva-wayland.pc; do
	grep -qx "$e" PLIST || echo "$e" >> PLIST
done
{ grep '^@comment' PLIST; grep -v '^@comment' PLIST | sort; } > PLIST.new && mv PLIST.new PLIST
make clean
make replace
make clean
ls -la /usr/pkg/lib/libva-wayland.so* /usr/pkg/lib/pkgconfig/libva-wayland.pc
