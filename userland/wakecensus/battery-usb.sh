#!/bin/sh
# battery-usb.sh [SECONDS]: whole-machine power on battery, panel off, for the
# three USB states: controllers on with runtime suspend, on without it, and
# `usb off` (controllers suspended).  Order a b c c b a.  Log /var/tmp/battery-usb.txt.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
W=${1:-300}; OUT=/var/tmp/battery-usb.txt
mh() { su mag -c "sh /var/tmp/fftest/mhctl.sh '$1'" >/dev/null 2>&1; }
state() {
	case "$1" in
	a) usb on >/dev/null 2>&1; sysctl -w hw.usbpm.autosuspend=5 >/dev/null ;;
	b) usb on >/dev/null 2>&1; sysctl -w hw.usbpm.autosuspend=0 >/dev/null; usbdevs >/dev/null 2>&1 ;;
	c) sysctl -w hw.usbpm.autosuspend=5 >/dev/null; usb off >/dev/null 2>&1 ;;
	esac
}
[ "$(envstat -d acpiacad0 | awk '/connected/{print $2}')" = TRUE ] && { echo "on the charger" >&2; exit 1; }
echo "=== $(date '+%F %T') start W=$W kernel $(sysctl -n machdep.booted_kernel)" >> $OUT
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank))"; sleep 20
for s in a b c c b a; do
	# the panel must be off for the whole run; re-blank if input brought it back
	mh "(mahogany::idle-blank)"; sleep 5
	state $s; sleep 25
	i0=$(sysctl -n machdep.lpsched.idle_ms)
	# no usbdevs here: with the controllers suspended (state c) a control
	# transfer blocks in the kernel for good
	echo "-- state $s usbpm=$(sysctl -n hw.usbpm.autosuspend) stats=$(sysctl -n hw.usbpm.stats) pipe=$(( ($(/var/tmp/memread 0x603c070000 0x008 2>/dev/null | awk '{print $NF}') >> 31) & 1 ))" >> $OUT
	wakecensus $W "usb-$s" 2>&1 | grep -E "^== wakecensus|  pc(3|10) |Total|battery" >> $OUT
	i1=$(sysctl -n machdep.lpsched.idle_ms); [ $((i1 - i0)) -lt $((W*1000 - 5000)) ] && echo "-- TOUCHED: discard" >> $OUT
done
state a
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-schedule))"
echo "=== $(date '+%F %T') done" >> $OUT
