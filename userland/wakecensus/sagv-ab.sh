#!/bin/sh
# sagv-ab.sh [SECONDS]: lispbsd_sagv 1 (driver-managed SAGV) vs 2 (memory
# pinned at the top QGV point), same load (leave a video looping), counter-
# balanced 1 2 2 1, wakecensus-energy with the rate sensor polled every 30 s.
# Log /var/tmp/sagv-ab.txt.  Ends in mode 1.  Battery, hands off.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
W=${1:-300}; OUT=/var/tmp/sagv-ab.txt
f() { sysctl -n hw.i915sagv | tr ' ' '\n' | grep "^$1=" | cut -d= -f2; }
echo "=== $(date '+%F %T') start W=$W kernel $(sysctl -n machdep.booted_kernel) ac=$(envstat -d acpiacad0 | awk '/connected/{print $2}')" >> $OUT
for m in 1 2 2 1; do
	sysctl -w hw.drm2.i915_modparams.lispbsd_sagv=$m >/dev/null; sleep 20
	echo "-- mode $m: applied=$(f applied) sagv_allowed=$(f sagv_allowed) mask=$(f mask) errors=$(f errors)" >> $OUT
	( i=0; while [ $i -lt $((W/5)) ]; do f sa_perf | cut -c9-10; sleep 5; i=$((i+1)); done | sort | uniq -c | tr '\n' ' ' > /var/tmp/sagv-ab.clk ) &
	wakecensus-energy -p 30 $W "sagv=$m" 2>&1 | grep -E '^== |battery:|rate sensor|TOUCHED|WARNING|pc10|pc8 ' >> $OUT
	wait; echo "   memory clock field (0x20 low, 0x40 top): $(cat /var/tmp/sagv-ab.clk)" >> $OUT
done
sysctl -w hw.drm2.i915_modparams.lispbsd_sagv=1 >/dev/null
echo "=== $(date '+%F %T') done, mode 1; dmesg underruns: $(dmesg | grep -ciE 'underrun|mismatch')" >> $OUT
