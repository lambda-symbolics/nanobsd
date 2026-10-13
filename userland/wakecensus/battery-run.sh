#!/bin/sh
# battery-run.sh: waits for the charger to be pulled, then measures whole-machine
# power per state with the battery's Wh counter over long windows.  Log:
# /var/tmp/battery-run.txt.  Laptop must stay untouched.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
OUT=/var/tmp/battery-run.txt; W=${W:-600}
mh() { su mag -c "sh /var/tmp/fftest/mhctl.sh '$1'" >/dev/null 2>&1; }
ac() { envstat -d acpiacad0 | awk '/connected/{print $2}'; }
log() { echo "=== $(date '+%T') $*" >> $OUT; }
run() { wakecensus $W "$1" 2>&1 | grep -E "^== wakecensus|  pc(2|3|8|10) |Total|cpu0 timer|battery" >> $OUT; }
while [ "$(ac)" = TRUE ]; do sleep 15; done
log "charger out, starting; window ${W}s per state; brightness orig $(sysctl -n hw.acpi.acpiout15.brightness)"
orig=$(sysctl -n hw.acpi.acpiout15.brightness)
# --- panel off states ---
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank))"; sleep 30
sysctl -w kern.callout_deferrable=1 >/dev/null; sysctl -w hw.hdaudio.power_save=10 >/dev/null; sleep 20
run "panel-off deferrable=1 audio-pm=on"
sysctl -w kern.callout_deferrable=0 >/dev/null; sleep 10
run "panel-off deferrable=0 audio-pm=on"
sysctl -w kern.callout_deferrable=1 >/dev/null
sysctl -w hw.hdaudio.power_save=0 >/dev/null; mixerctl -w outputs.master=$(mixerctl -n outputs.master) >/dev/null 2>&1; sleep 10
run "panel-off deferrable=1 audio-pm=off(awake)"
sysctl -w hw.hdaudio.power_save=10 >/dev/null
# --- panel on, brightness curve ---
mh "(progn (setf mahogany::*idle-dim-seconds* nil mahogany::*idle-blank-seconds* 7200) (mahogany::idle-poke))"; sleep 15
for b in 0 25 50 75 100; do
	brightness $b >/dev/null 2>&1; sleep 20
	run "panel-on brightness=$b"
done
brightness $orig >/dev/null 2>&1
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-schedule))"
log "done; brightness restored to $orig"
