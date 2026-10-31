#!/bin/sh
# headless test of mhcu pointer commands against a GTK3 probe (cu-probe)
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/bin:/bin:/usr/sbin:/sbin
pkill -f '^/root/common-lisp/mahogany/build/mahogany' ; sleep 1
rm -rf /var/tmp/mhrun; mkdir -m 700 /var/tmp/mhrun; rm -f /tmp/probe.log
/var/tmp/mhtest.sh > /var/tmp/mhcu-mh.log 2>&1 &
i=0; while [ ! -S /var/tmp/mhrun/mahogany.sock ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
export MAHOGANY_SOCKET=/var/tmp/mhrun/mahogany.sock
sleep 2
mhcu launch "GDK_BACKEND=wayland /var/tmp/cu-probe > /tmp/probe.log 2>/tmp/probe.err" >/dev/null
w=$(mhcu wait cu-probe 20)
set -- $(echo "$w" | sed 's/.*"x":\([0-9]*\),"y":\([0-9]*\),"width":\([0-9]*\),"height":\([0-9]*\).*/\1 \2 \3 \4/')
x=$1 y=$2 W=$3 H=$4; echo "probe at $x $y ${W}x$H"
cy=$((y + H / 2)); lx=$((x + W / 4)); rx=$((x + 3 * W / 4))
sleep 1
mark() { echo "--- $*" >> /tmp/probe.log; echo "== $*"; }
mark "unfocused ctrl+shift click"; mhcu click $rx $cy --mods ctrl+shift; sleep 0.3
mark "focused ctrl+shift click"; mhcu click $rx $cy --mods ctrl+shift; sleep 0.3
mark "triple click"; mhcu click $rx $cy left 3; sleep 0.3
mark "double click";     mhcu dclick $rx $cy; sleep 0.3
mark "right click alt";  mhcu click $rx $cy right --mods alt; sleep 0.3
mark "wheel 2";          mhcu scroll $rx $cy 2; sleep 0.3
mark "pixels 37 down, 12 left"; mhcu scroll $rx $cy 37 -12 --pixels; sleep 0.3
mark "ctrl wheel up";    mhcu scroll $rx $cy -1 --mods ctrl; sleep 0.3
mark "drag path left->mid->right"; mhcu drag $lx $cy $((x + W / 2)) $((y + H / 4)) $rx $cy --pause 100; sleep 0.5
mark "abort a slow drag"; mhcu drag $lx $cy $rx $cy --step-ms 300 --nowait; sleep 0.5
mhcu busy; mhcu abort; mhcu busy; sleep 0.3
mark "end"
sleep 0.3
echo "======== probe log (motion lines counted)"
awk '/^motion/ { m++; next } { if (m) print "  motion x" m; m = 0; print }' /tmp/probe.log
cat /tmp/probe.err | grep -v Gtk-WARNING | head -5
pkill -f '^/root/common-lisp/mahogany/build/mahogany'
