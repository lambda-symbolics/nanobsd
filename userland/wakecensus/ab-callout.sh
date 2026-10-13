#!/bin/sh
# ab-callout.sh [SECONDS] [ROUNDS]: kern.callout_deferrable 1 vs 0, interleaved,
# wakecensus for each; appends to /var/tmp/ab-callout.txt.  Idle machine.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
n=${1:-60}; rounds=${2:-2}; OUT=/var/tmp/ab-callout.txt
orig=$(sysctl -n kern.callout_deferrable)
r=0
while [ $r -lt $rounds ]; do
	for mode in 1 0; do
		sysctl -w kern.callout_deferrable=$mode >/dev/null
		sleep 10
		wakecensus $n "deferrable=$mode round=$r" >> $OUT 2>&1
	done
	r=$((r+1))
done
sysctl -w kern.callout_deferrable=$orig >/dev/null
echo "=== $(date) done; deferred skips so far: $(sysctl -n kern.callout_deferred)" >> $OUT
