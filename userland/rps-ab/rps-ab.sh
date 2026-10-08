#!/bin/sh
# rps-ab.sh [REPS] [SECS]: GPU frequency policy A/B on kernel 137+.
# Workload: fullscreen mpv in the Mahogany session looping the 1440p60 VP9
# clip with zero-copy VA-API (plus vaapi-copy under the stock policy), panel
# on at 60 Hz, EPP fixed, refresh policy and lpschedd stopped.  Policies are
# interleaved per rep.  Per condition: package and graphics RAPL watts, PC
# states, GT RC6, mean actual GT MHz over the awake samples, RPS event
# counter deltas, compositor fps and mpv frame drops.  Appends to
# /var/tmp/rps/rps-ab.txt.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin:/usr/local/bin
T=/var/tmp/fftest; R=/var/tmp/rps; reps=${1:-3}; secs=${2:-60}
OUT=$R/rps-ab.txt
. $R/rps-lib.sh
echo "=== rps-ab $(date) $(uname -v | cut -d: -f1) ac=$(envstat -d acpiacad0 | awk '/connected/ {print $2}') reps=$reps secs=$secs" | tee -a $OUT
sysctl hw.i915rps.rp0_mhz hw.i915rps.rp1_mhz hw.i915rps.rpe_mhz hw.i915rps.rpn_mhz | tr '\n' ' ' | tee -a $OUT; echo | tee -a $OUT
/etc/rc.d/lpschedd stop > /dev/null 2>&1
sysctl -qw machdep.hwp.epp=240
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null
mh "(mahogany::refresh-stop)" > /dev/null
mh "(hrt:output-set-refresh (first (mahogany::idle-outputs)) 60000)" > /dev/null
policy stock; sleep 5
meas desktop-stock
policy timer; sleep 5
meas desktop-timer
policy stock
mpvstart vaapi
i=0
while [ $i -lt $reps ]; do
	for p in stock timer timer-nopd rpe cap400; do policy $p; sleep 10; meas mpv-vaapi-$p; done
	i=$((i + 1))
done
policy timer
mpvstop
mpvstart vaapi-copy
i=0
while [ $i -lt $reps ]; do sleep 10; meas mpv-vaapi-copy-timer; i=$((i + 1)); done
mpvstop
policy timer
mh "(mahogany::refresh-start)" > /dev/null
su mag -c "sh $T/mhform.sh $T/mh-restore.lisp" > /dev/null
/etc/rc.d/lpschedd start > /dev/null 2>&1
echo "=== rps-ab done $(date)" | tee -a $OUT
