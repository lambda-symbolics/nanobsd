#!/bin/sh
# headless computer-use test: never touches the user's session
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/bin:/bin:/usr/sbin:/sbin
pkill -f '^/root/common-lisp/mahogany/build/mahogany' ; sleep 1
rm -rf /var/tmp/mhrun; mkdir -m 700 /var/tmp/mhrun; rm -f /tmp/cu-out.txt /tmp/cu-shot*.png
/var/tmp/mhtest.sh > /var/tmp/mhcu-mh.log 2>&1 &
i=0; while [ ! -S /var/tmp/mhrun/mahogany.sock ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
export MAHOGANY_SOCKET=/var/tmp/mhrun/mahogany.sock
sleep 2
echo "== screen";   mhcu screen
echo "== launch";   mhcu launch "foot -o colors.background=203040 /bin/sh"
echo "== wait";     mhcu wait foot 15
echo "== windows";  mhcu windows
echo "== click";    mhcu click 500 400
echo "== type";     mhcu type 'echo "Hello, World! @#$%^&*()_+{}|:<>?~" > /tmp/cu-out.txt
'
sleep 1; echo "== file: $(cat /tmp/cu-out.txt 2>&1)"
echo "== key";      mhcu key ctrl+u "e" "c" "h" "o" space "x" shift+y Return
mhcu type 'echo second >> /tmp/cu-out.txt
'
sleep 1; echo "== file2:"; cat /tmp/cu-out.txt
echo "== scroll";   mhcu scroll 500 400 3
echo "== drag";     mhcu drag 100 100 400 300
echo "== shot";     mhcu shot /tmp/cu-shot.png 0.5
echo "== shot region"; mhcu shot /tmp/cu-shot2.png 1 0 0 400 300
echo "== focused";  mhcu focused
echo "== bad key";  mhcu key ctrl+nosuchkey
echo "== unicode";  mhcu type 'ž€'
echo "== keybinding"; mhcu key ctrl+t g n
sleep 0.5
grep -c 'handled by keybinding\|Key pressed' /var/tmp/mhcu-mh.log
grep -i 'computer use\|error' /var/tmp/mhcu-mh.log | head -5
echo "== find"; mhcu find foot; echo "== setclip"; mhcu setclip "clip \"x\" ok"; echo "== clip"; mhcu clip; echo "== empty skipped"; mhcu type a
pkill -f '^/root/common-lisp/mahogany/build/mahogany'
