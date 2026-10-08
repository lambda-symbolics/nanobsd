#!/bin/sh
# Build mpv from the local pkgsrc tree, not from binary packages: pkgin
# resolves mpv against the repository's newer libraries and upgrades
# everything that shares them, the locally built Wayland Firefox included.
# A source build only adds what is missing and stops if an installed package
# is too old instead of replacing it.  Never run pkgin or pkg_add while it runs.
#
# Local change to multimedia/mpv (pkgsrc-mpv-Makefile.diff):
#  - native Wayland (pkgsrc turns it off and routes Wayland through SDL2),
#    which needs wayland, wayland-protocols, libxkbcommon and input-headers
#    (linux/input-event-codes.h) buildlinked;
#  - ffmpeg8, the ffmpeg Firefox already uses, instead of a second ffmpeg7.
# USE_BUILTIN.MesaLib=no links pkgsrc's MesaLib (libEGL.so.1, with the
# Wayland platform) instead of the base X11 one (libEGL.so.0, X11 only):
# with the base one every GPU output fails and auto-probing then hits an
# assertion in mpv's X11 fallback.
# Options (/etc/mk.conf): no Blu-ray/sixel/JavaScript/SDL2/VDPAU, sndio for
# audio (libsndio talks to /dev/audio itself, no daemon).  libsndio needs the
# LISPBSD patch from userland/sndio, or every seek, pause and track switch
# hangs mpv (docs/mpv-sndio.org).
# Pulled in on 2026-10-07, nothing replaced: libdvdread, libdvdnav,
# libplacebo, vulkan-headers, py313-glad2, lua52, sndio and Python build tools.
# libva must be built with its Wayland backend first (userland/libva/build.sh):
# meson then finds libva-wayland and hwdec=vaapi (zero-copy, mpv.conf in
# dotfiles/) can open a VA display in the Wayland window.  REPLACE=1 rebuilds
# over an installed mpv (make replace) instead of package-install.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd /usr/pkgsrc/multimedia/mpv
[ -f Makefile.orig-lispbsd ] || { cp Makefile Makefile.orig-lispbsd; patch Makefile < "$here/pkgsrc-mpv-Makefile.diff"; }
grep -q 'PKG_OPTIONS.mpv' /etc/mk.conf ||
    printf 'PKG_OPTIONS.mpv=\t-bluray -sixel -javascript -sdl2 -vdpau sndio\n' >> /etc/mk.conf
make clean
if [ -n "$REPLACE" ]; then
	make replace USE_BUILTIN.MesaLib=no
else
	make package-install USE_BUILTIN.MesaLib=no
fi
make clean
