#!/bin/sh
# Build freezeshot: /usr/local/lib/libfreezeshot.so (C: Wayland capture,
# overlay, input, PNG) and /usr/local/bin/freezeshot (SBCL executable, the
# selection logic, sb-alien only).  Run as root on the laptop from here.
set -e
here=$(cd "$(dirname "$0")" && pwd)
SBCL=${SBCL:-/usr/local/bin/sbcl}
LIB=${LIB:-/usr/local/lib/libfreezeshot.so}
OUT=${OUT:-/usr/local/bin/freezeshot}
export PKG_CONFIG_PATH=/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig
build=$(mktemp -d /tmp/freezeshot.XXXXXX)
trap 'rm -rf "$build"' EXIT

for p in wlr-screencopy-unstable-v1 wlr-layer-shell-unstable-v1 xdg-shell \
    tablet-v2 cursor-shape-v1; do
	wayland-scanner client-header "$here/protocols/$p.xml" \
	    "$build/$p-client-protocol.h"
	wayland-scanner private-code "$here/protocols/$p.xml" \
	    "$build/$p-protocol.c"
done
clang -O2 -Wall -Wno-unused-parameter -fPIC -shared -I"$build" \
    $(pkg-config --cflags wayland-client cairo) \
    -o "$LIB.new" "$here/freezeshot-core.c" "$build"/*-protocol.c \
    $(pkg-config --libs wayland-client cairo) \
    -Wl,-rpath,/usr/pkg/lib -Wl,-rpath,/usr/X11R7/lib
mv "$LIB.new" "$LIB"

"$SBCL" --non-interactive --no-userinit \
  --eval "(sb-alien:load-shared-object \"$LIB\")" \
  --eval "(load (compile-file \"$here/freezeshot.lisp\" :output-file \"$build/freezeshot.fasl\"))" \
  --eval "(sb-ext:save-lisp-and-die \"$OUT.new\" :toplevel #'freezeshot:main :executable t :save-runtime-options t)"
mv "$OUT.new" "$OUT"
ls -l "$LIB" "$OUT"
