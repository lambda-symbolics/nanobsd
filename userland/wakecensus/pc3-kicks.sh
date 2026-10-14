#!/bin/sh
# pc3-kicks.sh: panel off, idle: measure package residency, then one candidate
# "kick" at a time (net burst, disk I/O, a modeset round trip), measuring after
# each, to find what moves the package out of the sticky PC3 state.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
OUT=/var/tmp/pc3-kicks.txt
mh() { su mag -c "sh /var/tmp/fftest/mhctl.sh '$1'" >/dev/null 2>&1; }
m() { wakecensus 60 "$1" 2>&1 | grep -E "^== wakecensus|  pc(3|10) |battery" >> $OUT; }
pc3() { tail -4 $OUT | awk '/  pc3 /{print int($2)}' | tail -1; }
echo "=== $(date '+%F %T') start ac=$(envstat -d acpiacad0 | awk '/connected/{print $2}')" >> $OUT
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank))"; sleep 30
m "baseline"
[ "$(pc3)" -lt 20 ] && { echo "baseline PC3 low; nothing to kick" >> $OUT; exit 0; }
echo "-- net kick (60 MB over the tunnel)" >> $OUT
ssh -o ConnectTimeout=10 -o BatchMode=yes root@10.0.0.1 "head -c 60000000 /dev/zero" > /dev/null 2>&1; sleep 15; m "after-net"
[ "$(pc3)" -lt 20 ] && { echo "net kick cleared it" >> $OUT; exit 0; }
echo "-- disk kick (1 GB read, 200 MB write+sync)" >> $OUT
dd if=/dev/rdk1 of=/dev/null bs=1m count=1000 2>/dev/null; dd if=/dev/zero of=/var/tmp/kick.bin bs=1m count=200 2>/dev/null; sync; rm -f /var/tmp/kick.bin; sleep 15; m "after-disk"
[ "$(pc3)" -lt 20 ] && { echo "disk kick cleared it" >> $OUT; exit 0; }
echo "-- modeset kick (panel on 5 s, off)" >> $OUT
mh "(mahogany::idle-unblank)"; sleep 5; mh "(mahogany::idle-blank)"; sleep 20; m "after-modeset"
[ "$(pc3)" -lt 20 ] && { echo "modeset kick cleared it" >> $OUT; exit 0; }
echo "nothing cleared it" >> $OUT
