#!/bin/sh
# run.sh LABEL: headless Mahogany from the build tree, two keylog windows,
# super+Right and super+Left through the computer-use keyboard.
L=$1; R=/var/tmp/kltest; rm -rf $R; mkdir -p $R/cfg/mahogany $R/run; chmod 700 $R/run
cat > $R/cfg/mahogany/init.lisp <<'X'
(in-package #:mahogany)
(define-key *top-map* (kbd "s-Left") #'strip-focus-left)
(define-key *top-map* (kbd "s-Right") #'strip-focus-right)
X
export XDG_RUNTIME_DIR=$R/run XDG_CONFIG_HOME=$R/cfg
export MAHOGANY_SOCKET=$R/run/mahogany.sock
( export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
  export LD_LIBRARY_PATH=/root/common-lisp/mahogany/build/heart
  exec /root/common-lisp/mahogany/build/mahogany > $R/mh.log 2>&1 ) &
MH=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -S $R/run/mahogany.sock ] && break; sleep 0.5; done
sleep 1
export WAYLAND_DISPLAY=$(ls $R/run | grep '^wayland-[0-9]*$' | head -1)
/var/tmp/kl/keylog A >> $R/A.log 2>&1 & A=$!
sleep 1
/var/tmp/kl/keylog B >> $R/B.log 2>&1 & B=$!
m() { echo "$1"; echo "$1" >> $R/A.log; echo "$1" >> $R/B.log; }
sleep 1
echo "focused: $(/usr/bin/timeout 5 /usr/local/bin/mhcu focused)"
m "--- super+Left"; /usr/bin/timeout 5 /usr/local/bin/mhcu key super+Left; sleep 0.5
echo "focused: $(/usr/bin/timeout 5 /usr/local/bin/mhcu focused)"
m "--- super+Right"; /usr/bin/timeout 5 /usr/local/bin/mhcu key super+Right; sleep 0.5
echo "focused: $(/usr/bin/timeout 5 /usr/local/bin/mhcu focused)"
m "--- held super, Left down, 0.5 s, Left up, super up"
C=/usr/local/bin/mhcu
$C hold super down >/dev/null; $C hold Left down >/dev/null; sleep 0.5; $C hold Left up >/dev/null; sleep 0.3; $C hold super up >/dev/null; sleep 0.5
echo "focused: $(/usr/bin/timeout 5 /usr/local/bin/mhcu focused)"
m "--- plain Right"; /usr/bin/timeout 5 /usr/local/bin/mhcu key Right; sleep 0.5
kill $A $B; sleep 0.3; kill $MH; sleep 1; kill -9 $MH 2>/dev/null
echo "=== $L A:"; cat $R/A.log; echo "=== $L B:"; cat $R/B.log
