#!/bin/sh
# rps-ff.sh [REPS] [SECS]: GPU frequency policy A/B with the fftest Firefox
# animation page (WebRender on iris, 60 Hz): stock (1100 MHz) vs the
# busyness timer, interleaved.  Same measurement as rps-ab.sh; the page's
# own frame rate is the compositor fps.  Appends to /var/tmp/rps/rps-ab.txt.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin:/usr/local/bin
T=/var/tmp/fftest; R=/var/tmp/rps; reps=${1:-2}; secs=${2:-45}
OUT=$R/rps-ab.txt
. $R/rps-lib.sh
echo "=== rps-ff $(date) $(uname -v | cut -d: -f1) ac=$(envstat -d acpiacad0 | awk '/connected/ {print $2}') reps=$reps secs=$secs" | tee -a $OUT
/etc/rc.d/lpschedd stop > /dev/null 2>&1
sysctl -qw machdep.hwp.epp=240
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null
mh "(mahogany::refresh-stop)" > /dev/null
mh "(hrt:output-set-refresh (first (mahogany::idle-outputs)) 60000)" > /dev/null
su mag -c "sh $T/mhgrp.sh 6" > /dev/null; sleep 2
su mag -c "sh $T/launch.sh wayland file://$T/anim.html"; sleep 25
i=0
while [ $i -lt $reps ]; do
	for p in stock timer; do policy $p; sleep 10; meas ff-anim-$p; done
	i=$((i + 1))
done
sh $T/ffkill.sh; sleep 3
policy timer
mh "(mahogany::refresh-start)" > /dev/null
su mag -c "sh $T/mhform.sh $T/mh-restore.lisp" > /dev/null
/etc/rc.d/lpschedd start > /dev/null 2>&1
echo "=== rps-ff done $(date)" | tee -a $OUT
