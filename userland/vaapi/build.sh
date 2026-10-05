#!/bin/sh
# Build Intel's VA-API driver (iHD) for the Tiger Lake GPU on NetBSD and
# install it in /usr/local (see docs/vaapi.org).  Needs kernel 133 or later.
# Run as root from this directory.  Versions follow FreeBSD's ports:
# gmmlib 22.10.0 and media-driver 26.1.5, with the patches FreeBSD applies
# (upstream PRs gmmlib#68 and media-driver#1785, plus its files/ patches)
# kept in patches/, and two NetBSD fixes applied below.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-/var/tmp/va}
export PATH=$PATH:/usr/pkg/bin
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig
mkdir -p "$work" && cd "$work"
[ -f gmmlib-22.10.0.tar.gz ] ||
    ftp -o gmmlib-22.10.0.tar.gz https://github.com/intel/gmmlib/archive/refs/tags/intel-gmmlib-22.10.0.tar.gz
[ -f media-driver-26.1.5.tar.gz ] ||
    ftp -o media-driver-26.1.5.tar.gz https://github.com/intel/media-driver/archive/refs/tags/intel-media-26.1.5.tar.gz

# gmmlib
rm -rf gmmlib-intel-gmmlib-22.10.0 && tar xzf gmmlib-22.10.0.tar.gz
cd gmmlib-intel-gmmlib-22.10.0
for p in "$here"/patches/gmmlib/*.patch; do patch -p1 -s < "$p"; done
mkdir build && cd build
CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release -DRUN_TEST_SUITE=OFF \
    -DCMAKE_INSTALL_RPATH=/usr/local/lib
gmake -j8 && gmake install
cd "$work"

# media-driver
rm -rf media-driver-intel-media-26.1.5 && tar xzf media-driver-26.1.5.tar.gz
cd media-driver-intel-media-26.1.5
for p in "$here"/patches/media-driver/*.patch; do patch -p1 -s -F 3 < "$p"; done
for p in "$here"/patches/freebsd/patch-*; do patch -p0 -s < "$p"; done
# NetBSD: no <linux/types.h> either, and no libdl (dlopen is in libc).
sed -i 's/^#if defined(__FreeBSD__)$/#if defined(__FreeBSD__) || defined(__NetBSD__)/' \
    media_softlet/linux/common/os/xe/include/dma-buf.h
sed -i 's|m pthread dl")|m pthread ${CMAKE_DL_LIBS}")|' media_driver/media_top_cmake.cmake
mkdir build && cd build
inc="-I/usr/local/include -I/usr/pkg/include -I/usr/X11R7/include"
libs="-L/usr/local/lib -Wl,-R/usr/local/lib -L/usr/pkg/lib -Wl,-R/usr/pkg/lib -L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib"
# All platforms: trimming GPU generations breaks the build (Xe2 includes
# Xe_HPG headers).  cmake 4 needs the policy minimum.
CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS="$inc" -DCMAKE_CXX_FLAGS="$inc" \
    -DCMAKE_SHARED_LINKER_FLAGS="$libs" -DCMAKE_EXE_LINKER_FLAGS="$libs" \
    -DMEDIA_BUILD_FATAL_WARNINGS=OFF -DBUILD_CMRTLIB=OFF \
    -DMEDIA_RUN_TEST_SUITE=OFF -DARCH=64 -DBUILD_TYPE=Release
gmake -j8
# Not `gmake install`: it takes libva's colon-separated driver path as one
# directory name.
mkdir -p /usr/local/lib/dri
install -m 755 media_driver/iHD_drv_video.so /usr/local/lib/dri/
# libva looks in /usr/pkg/lib/dri; Firefox dlopens the Linux soname
# libdrm.so.2, NetBSD's libdrm is .so.3.
ln -sf /usr/local/lib/dri/iHD_drv_video.so /usr/pkg/lib/dri/iHD_drv_video.so
ln -sf /usr/X11R7/lib/libdrm.so.3 /usr/local/lib/libdrm.so.2
