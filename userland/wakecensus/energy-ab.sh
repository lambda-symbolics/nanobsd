#!/bin/sh
# energy-ab.sh [SECONDS] [ROUNDS]: kern.timecounter.hardware TSC vs TSC-low on
# battery with the panel blanked, counterbalanced (TSC TSC-low TSC-low TSC per
# round), one wakecensus-energy window per condition.  Log:
# /var/tmp/energy-ab.txt.  Run detached, hands off, after 10 min of uptime.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
W=${1:-600}; R=${2:-1}; OUT=/var/tmp/energy-ab.txt
mh() { su mag -c "sh /var/tmp/fftest/mhctl.sh '$1'" >/dev/null 2>&1; }
ac() { envstat -d acpiacad0 | awk '/connected/{print $2}'; }
[ "$(ac)" = TRUE ] && { echo "on the charger; this run wants the battery" >&2; exit 1; }
tc0=$(sysctl -n kern.timecounter.hardware)
echo "=== $(date '+%F %T') start W=$W R=$R kernel $(sysctl -n machdep.booted_kernel) uptime $(uptime | sed 's/.*up //; s/,.*//') usb: $(usb status 2>/dev/null)" >> $OUT
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank))"; sleep 30
r=0
while [ $r -lt $R ]; do
	for tc in TSC TSC-low TSC-low TSC; do
		sysctl -w kern.timecounter.hardware=$tc >/dev/null; sleep 30
		wakecensus-energy $W "tc=$tc round=$r" 2>&1 | grep -E '^== |battery:|TOUCHED|WARNING|pc10|pc8 |cpu0 timer' >> $OUT
	done
	r=$((r+1))
done
sysctl -w kern.timecounter.hardware=$tc0 >/dev/null
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-schedule))"
echo "=== $(date '+%F %T') done (timecounter back to $tc0)" >> $OUT
