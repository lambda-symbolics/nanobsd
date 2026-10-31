#!/bin/sh
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/bin:/bin:/usr/sbin:/sbin
pkill -f '^/root/common-lisp/mahogany/build/mahogany' ; sleep 1
rm -rf /var/tmp/mhrun; mkdir -m 700 /var/tmp/mhrun; rm -f /tmp/cu-cz.txt
/var/tmp/mhtest.sh > /var/tmp/mhcu-mh.log 2>&1 &
i=0; while [ ! -S /var/tmp/mhrun/mahogany.sock ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
export MAHOGANY_SOCKET=/var/tmp/mhrun/mahogany.sock
sleep 2
mhcu eval '(xkb:with-xkb-rule-names (rules (:layout "cz,us" :options "grp:alt_shift_toggle")) (mahogany-set-keymap *compositor-state* :rules rules))'
mhcu launch "LANG=cs_CZ.UTF-8 LC_ALL=C.UTF-8 foot /bin/sh"
echo "== wait prefix"; mhcu wait foo 15 | cut -c1-80
mhcu click 300 300 >/dev/null
echo "== type cz"; mhcu type 'printf "%s\n" "me@mag.wiki ž ěščřý 2+3=5 {[|]} <>#&\$~" > /tmp/cu-cz.txt
'
sleep 1; echo "== file: $(cat /tmp/cu-cz.txt)"
echo "== chord"; mhcu key ctrl+u
echo "== switch to us (alt+shift) and type"; mhcu key alt+shift_l
mhcu type 'echo "us@layout ok" >> /tmp/cu-cz.txt
'
sleep 1; echo "== file2:"; cat /tmp/cu-cz.txt
pkill -f '^/root/common-lisp/mahogany/build/mahogany'
