#!/bin/sh
# Headless test of Mahogany's output mirroring (lisp/mirror.lisp, heart
# mirror.c): two virtual 1280x720 outputs, a red foot window on the first,
# mirror it onto the second in every fit and check pixels of what the
# target shows (captured with (mirror-command "shot"); grim cannot see a
# target, it is out of the layout).  Then stop, mirror again, destroy the
# target and check the compositor survives.  Needs pngtopnm and python3.13.
# Run as root on the laptop: sh mirror-test.sh [MAHOGANY-CHECKOUT]
M=${1:-/root/common-lisp/mahogany}
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/bin:/bin:/usr/sbin:/sbin
export XDG_RUNTIME_DIR=/var/tmp/mhrun XDG_CONFIG_HOME=/var/tmp/mhcfg
export MAHOGANY_SOCKET=/var/tmp/mhrun/mahogany.sock WAYLAND_DISPLAY=wayland-0
HERE=$(cd "$(dirname "$0")" && pwd)
T=/var/tmp/mirror-test
rm -rf /var/tmp/mirror-test; mkdir -p $T $XDG_RUNTIME_DIR $XDG_CONFIG_HOME/mahogany
[ -f $XDG_CONFIG_HOME/mahogany/init.lisp ] ||
	echo '(in-package #:mahogany) (bar-start)' > $XDG_CONFIG_HOME/mahogany/init.lisp
pkill -f "^$M/build/mahogany"; sleep 0.5
rm -f $MAHOGANY_SOCKET
( export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
  export LD_LIBRARY_PATH=$M/build/heart
  exec $M/build/mahogany -l DEBUG ) > $T/mh.log 2>&1 &
i=0; while [ ! -S $MAHOGANY_SOCKET ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
sleep 1

fails=0
ctl() { mahoganyctl "$1" | sed 's/\\"/"/g'; }
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
shot() {
	ctl "(mirror-command \"shot\" \"$T/$1.png\")" >/dev/null
	i=0; while [ ! -s $T/$1.png ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
	pngtopnm $T/$1.png > $T/$1.ppm 2>/dev/null
}
# check NAME SIZE COLOUR X Y [COLOUR X Y ...]: the capture NAME is SIZE and
# has those pixels (r = the red window, k = black)
check() {
	name=$1 size=$2; shift 2
	args= want=
	while [ $# -ge 3 ]; do
		case $1 in r) c=c03030 ;; k) c=000000 ;; esac
		args="$args $2 $3"; want="$want ($2,$3)=$c"; shift 3
	done
	got=$(python3.13 $HERE/ppmpix.py $T/$name.ppm $args)
	[ "$got" = "$size$want" ] && pass "$name" || fail "$name: want $size$want, got $got"
}

foot -o colors.background=c03030 -o colors.foreground=ffffff sh -c 'echo MIRROR TEST; sleep 600' >/dev/null 2>&1 &
sleep 2

ctl '(mirror-command "start" "HEADLESS-2" "HEADLESS-1")' | grep -q '"target":"HEADLESS-2"' &&
	pass "start" || fail "start"
sleep 0.5
grim -o HEADLESS-1 $T/source.png; pngtopnm $T/source.png > $T/source.ppm
shot same
cmp -s $T/source.ppm $T/same.ppm && pass "same size shows the source exactly" ||
	fail "same size differs from the source"
grim -o HEADLESS-2 $T/nope.png 2>/dev/null && fail "grim still sees the target" ||
	pass "target is out of the layout"

# The window is x 16-632, y 33-703 on the 1280x720 source.
ctl '(progn (hrt:output-configure (mahogany::%mirror-find-output "HEADLESS-2") (hrt:make-output-config :dimensions (cons 800 600) :custom-mode t :enabled t)) (mahogany::%mirror-output-change) t)' >/dev/null
sleep 0.3; shot contain   # 800x450 at y 75, window x 10-395 y 95-514
check contain 800x600 r 200 300 k 600 300 k 200 40 k 200 560
ctl '(mirror-command "fit" "cover")' >/dev/null; sleep 0.3; shot cover   # x from 160
check cover 800x600 r 200 300 k 600 300 r 5 580
ctl '(mirror-command "fit" "stretch")' >/dev/null; sleep 0.3; shot stretch
check stretch 800x600 r 200 300 k 600 300 r 200 580
ctl '(mirror-command "fit" "exact")' >/dev/null; sleep 0.3; shot exact   # x 240-1040
check exact 800x600 r 200 300 k 600 300 r 0 0
ctl '(progn (mirror-command "fit" "contain") (mirror-command "region" 0 0 320 180))' >/dev/null
sleep 0.3; shot region    # 2.5x: window from x 40, y 157
check region 800x600 r 400 300 k 20 300 k 400 100
ctl '(mirror-command "region")' >/dev/null

ctl '(mirror-command "freeze")' | grep -q '"frozen":true' && pass "freeze" || fail "freeze"
ctl '(mirror-command "thaw")' | grep -q '"frozen":false' && pass "thaw" || fail "thaw"

ctl '(mirror-command "stop")' | grep -q '"mirrors":\[\]' && pass "stop" || fail "stop"
grim -o HEADLESS-2 $T/after.png && pass "target back in the layout" || fail "target not back"

ctl '(mirror-command "start")' | grep -q '"target":"HEADLESS-2"' && pass "start again" ||
	fail "start again"
ctl '(let ((o (mahogany::%mirror-find-output "HEADLESS-2"))) (cffi:foreign-funcall "wlr_output_destroy" :pointer (cffi:mem-ref (hrt:output-hrt-output o) :pointer) :void) t)' >/dev/null
sleep 0.5
ctl '(mirror-status)' | grep -q '"mirrors":\[\]' && pass "target unplugged ends the mirror" ||
	fail "mirror survived its target"
ctl '(mirror-command "start")' | grep -q 'no other output' && pass "one output: refused" ||
	fail "one output"
pgrep -f "^$M/build/mahogany" >/dev/null && pass "compositor alive" || fail "compositor died"
pkill -f "^$M/build/mahogany"
echo "$fails failed"
exit $fails
