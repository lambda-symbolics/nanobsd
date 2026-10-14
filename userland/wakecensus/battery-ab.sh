#!/bin/sh
# battery-ab.sh [SECONDS] [ROUNDS]: counterbalanced 2x2 of kern.callout_deferrable
# and hw.hdaudio.power_save on battery, panel off, with the audio D-state
# verified before each window.  Whole-machine Wh from the battery counter is
# the result; package residency alongside.  Log: /var/tmp/battery-ab.txt.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
W=${1:-300}; R=${2:-2}; OUT=/var/tmp/battery-ab.txt
mh() { su mag -c "sh /var/tmp/fftest/mhctl.sh '$1'" >/dev/null 2>&1; }
hda() { pcictl pci0 dump -d 31 -f 3 2>/dev/null | awk '/Power state:/{print $3; exit}'; }
ac() { envstat -d acpiacad0 | awk '/connected/{print $2}'; }
[ "$(ac)" = TRUE ] && { echo "on the charger; this run wants the battery" >&2; exit 1; }
echo "=== $(date '+%F %T') start W=$W R=$R kernel $(sysctl -n machdep.booted_kernel)" >> $OUT
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank))"; sleep 30
# order: ABCD then DCBA, so each condition sits once early and once late
order="d1a1 d0a1 d1a0 d0a0"; r=0
while [ $r -lt $R ]; do
	for c in $order; do
		d=${c#d}; d=${d%a*}; a=${c#*a}
		sysctl -w kern.callout_deferrable=$d >/dev/null
		if [ "$a" = 1 ]; then sysctl -w hw.hdaudio.power_save=10 >/dev/null; else sysctl -w hw.hdaudio.power_save=0 >/dev/null; fi
		sleep 25
		echo "-- condition deferrable=$d audio-pm=$a hda=$(hda) pipe=$(( ($(/var/tmp/memread 0x603c070000 0x008 2>/dev/null | awk '{print $NF}') >> 31) & 1 ))" >> $OUT
		wakecensus $W "deferrable=$d audio-pm=$a round=$r" 2>&1 | grep -E "^== wakecensus|  pc(3|10) |Total|cpu0 timer|battery" >> $OUT
	done
	order=$(echo $order | tr ' ' '\n' | tail -r | tr '\n' ' ')
	r=$((r+1))
done
sysctl -w kern.callout_deferrable=1 >/dev/null; sysctl -w hw.hdaudio.power_save=10 >/dev/null
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-schedule))"
echo "=== $(date '+%F %T') done" >> $OUT
