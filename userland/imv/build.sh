#!/bin/sh
# Build imv (the Wayland image viewer that replaces feh) from the local
# pkgsrc tree, never pkgin (see userland/mpv/build.sh for why).
#
# USE_BUILTIN.MesaLib=no is required: with X11_TYPE=native pkgsrc links the
# base X11 Mesa (libEGL.so.0, X11 platform only) and imv-wayland segfaults in
# eglInitialize -> dri2_initialize_x11 -> XGetXCBConnection under Mahogany.
# pkgsrc's MesaLib (libEGL.so.1) has the Wayland platform.
# Pulled in on 2026-10-30: freeimage, inih, desktop-file-utils.
#
# /usr/local/bin/feh is a symlink to /usr/pkg/bin/imv (feh is X11-only and
# was removed).  REPLACE=1 rebuilds over an installed imv.
set -e
cd /usr/pkgsrc/graphics/imv
if [ "${REPLACE:-0}" = 1 ]; then
	make clean
	make replace USE_BUILTIN.MesaLib=no
else
	make install USE_BUILTIN.MesaLib=no
fi
ldd /usr/pkg/bin/imv-wayland | grep -q 'libEGL.so.1 => /usr/pkg/lib' ||
	{ echo "imv-wayland is not linked against pkgsrc libEGL" >&2; exit 1; }
ln -sf /usr/pkg/bin/imv /usr/local/bin/feh
