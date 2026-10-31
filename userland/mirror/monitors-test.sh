#!/bin/sh
# Headless test of Mahogany with two monitors, driven like a user through the
# computer-use pointer and keyboard (mhcu): the pointer moving onto an empty
# monitor focuses it, so Super+Return opens the terminal there; Super+Shift+
# arrows focus a monitor; Super+Ctrl+Shift+arrows move the column there.
# Run as root on the laptop: sh monitors-test.sh [MAHOGANY-CHECKOUT]
M=${1:-/root/common-lisp/mahogany}
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/bin:/bin:/usr/sbin:/sbin
export XDG_RUNTIME_DIR=/var/tmp/mhrun XDG_CONFIG_HOME=/var/tmp/mhmon
export MAHOGANY_SOCKET=/var/tmp/mhrun/mahogany.sock WAYLAND_DISPLAY=wayland-0
T=/var/tmp/monitors-test
rm -rf /var/tmp/monitors-test /var/tmp/mhmon; mkdir -p $T $XDG_RUNTIME_DIR $XDG_CONFIG_HOME/mahogany
cat > $XDG_CONFIG_HOME/mahogany/init.lisp <<'L'
(in-package #:mahogany)
;; As in the user's init.lisp: focus follows the mouse.
(config-system:set-config keyboard-focus-type :sloppy)
(defcommand test-terminal () (:method () (uiop:launch-program "foot -o colors.background=203040")))
(defcommand test-broken () (:method () (funcall 'no-such-function)))
(dolist (b '(("s-Return" test-terminal) ("s-F12" test-broken)
             ("s-S-Left" focus-monitor-left) ("s-S-Right" focus-monitor-right)
             ("s-C-S-Left" move-column-to-monitor-left)
             ("s-C-S-Right" move-column-to-monitor-right)))
  (define-key *top-map* (kbd (first b)) (symbol-function (second b))))
L
pkill -f "^$M/build/mahogany"; sleep 0.5; rm -f $MAHOGANY_SOCKET
( export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
  export LD_LIBRARY_PATH=$M/build/heart
  exec $M/build/mahogany -l DEBUG ) > $T/mh.log 2>&1 &
i=0; while [ ! -S $MAHOGANY_SOCKET ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
sleep 1

fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
out() { mahoganyctl '(hrt:output-name (group-current-output (state-current-group *compositor-state*)))' | tr -d '"'; }
# the x of every window, sorted by id: windows on HEADLESS-2 have x >= 1280
xs() { mhcu windows | tr '{' '\n' | sed -n 's/.*"x":\([-0-9]*\).*/\1/p' | tr '\n' ' '; }
nwin() { mhcu windows | tr '{' '\n' | grep -c '"id"'; }
waitwin() { i=0; while [ "$(nwin)" -lt $1 ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; sleep 0.5; }

mhcu screen | sed 's/.*"outputs"/outputs/' | cut -c1-200
mhcu move 640 360 >/dev/null; sleep 0.2
mhcu key super+Return >/dev/null; waitwin 1
x=$(xs); echo "one window, x: $x"
[ "$(out)" = HEADLESS-1 ] && pass "focus on HEADLESS-1 with the pointer there" || fail "focus $(out)"

mhcu move 1900 360 >/dev/null; sleep 0.3
[ "$(out)" = HEADLESS-2 ] && pass "pointer onto the empty HEADLESS-2 focuses it" ||
	fail "pointer onto HEADLESS-2: focus still on $(out)"
mhcu key super+Return >/dev/null; waitwin 2
x=$(xs); echo "two windows, x: $x"
case " $x" in *" 1"[2-9][0-9][0-9]*|*" "[2-9][0-9][0-9][0-9]*) pass "Super+Return opened the terminal on HEADLESS-2" ;;
	*) fail "the new terminal is not on HEADLESS-2 (x: $x)" ;; esac

mhcu move 300 360 >/dev/null; sleep 0.3
[ "$(out)" = HEADLESS-1 ] && pass "pointer back over the window on HEADLESS-1 focuses it" ||
	fail "focus $(out)"
mhcu key super+shift+Right >/dev/null; sleep 0.3
[ "$(out)" = HEADLESS-2 ] && pass "Super+Shift+Right focuses HEADLESS-2" || fail "Super+Shift+Right: $(out)"
mhcu key super+shift+Left >/dev/null; sleep 0.3
[ "$(out)" = HEADLESS-1 ] && pass "Super+Shift+Left focuses HEADLESS-1" || fail "Super+Shift+Left: $(out)"

mhcu key super+shift+Right >/dev/null; sleep 0.2
mhcu key super+ctrl+shift+Left >/dev/null; sleep 0.5
x=$(xs); echo "after moving the column left, x: $x"
case " $x" in *" 1"[2-9][0-9][0-9]*|*" "[2-9][0-9][0-9][0-9]*) fail "a window is still on HEADLESS-2 (x: $x)" ;;
	*) pass "Super+Ctrl+Shift+Left moved the column to HEADLESS-1" ;; esac
[ "$(out)" = HEADLESS-1 ] && pass "the focus went with the column" || fail "focus $(out)"
mhcu key super+ctrl+shift+Right >/dev/null; sleep 0.5
x=$(xs); echo "after moving it back right, x: $x"
case " $x" in *" 1"[2-9][0-9][0-9]*|*" "[2-9][0-9][0-9][0-9]*) pass "Super+Ctrl+Shift+Right moved it to HEADLESS-2" ;;
	*) fail "nothing on HEADLESS-2 (x: $x)" ;; esac
mhcu key super+F12 >/dev/null; sleep 0.3
pgrep -f "^$M/build/mahogany" >/dev/null && pass "a failing command leaves the session running" ||
	fail "a failing command ended the session"
mhcu shot $T/end.png 0.5 >/dev/null
pgrep -f "^$M/build/mahogany" >/dev/null && pass "compositor alive" || fail "compositor died"
grep -i 'error\|debugger' $T/mh.log | grep -v 'session.c\|Failed to open' | tail -5
pkill -f "^$M/build/mahogany"; pkill -f 'foot -o colors.background=203040'
echo "$fails failed"
exit $fails
