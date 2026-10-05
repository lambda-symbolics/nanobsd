#!/bin/sh
# epp-build.sh [REPS]: like epp-ab.sh, but only the build, repeated six
# times per run (about 25-45 s of load) so wall time resolves, at finer EPP
# steps.  Results: /var/tmp/epp-ab.txt.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin:/usr/local/bin
OUT=/var/tmp/epp-ab.txt
WPA=/var/tmp/wpa211/wpa_supplicant-2.11/wpa_supplicant
reps=${1:-2}
rapl() { sysctl -n machdep.cidle.rapl | sed 's/.*raw=\([0-9]*\).*/\1/'; }
joules() { echo "$1 $2" | awk '{ d = $2 - $1; if (d < 0) d += 4294967296; printf "%.1f", d * 61.035 / 1e6 }'; }
ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000' 2>/dev/null || echo $(( $(date +%s) * 1000 )); }
temp() { envstat -d coretemp0 2>/dev/null | awk '/temperature/ { print $3 + 0; exit }'; }
run() {
	sysctl -qw machdep.hwp.epp=$1
	sleep 30	# cool down between runs at the new EPP, idle
	e=0; t=0; k=0
	while [ $k -lt 6 ]; do
		(cd $WPA && gmake clean >/dev/null 2>&1); sync
		r0=$(rapl); t0=$(ms)
		(cd $WPA && gmake -j8 wpa_supplicant >/dev/null 2>&1)
		r1=$(rapl); t1=$(ms)
		e=$(echo "$e $(joules $r0 $r1)" | awk '{print $1 + $2}'); t=$((t + t1 - t0)); k=$((k + 1))
	done
	echo "$(date +%T) build6 epp=$1 secs=$(echo $t | awk '{printf "%.2f", $1/1000}') pkgJ=$e temp=$(temp)" >> $OUT
	sysctl -qw machdep.hwp.epp=240
}
echo "=== epp-build $(date) reps=$reps" >> $OUT
/etc/rc.d/lpschedd stop >/dev/null 2>&1
i=0
while [ $i -lt $reps ]; do
	[ $((i % 2)) = 0 ] && order="128 160 192 224 240" || order="240 224 192 160 128"
	for e in $order; do run $e; done
	i=$((i + 1))
done
/etc/rc.d/lpschedd start >/dev/null 2>&1
echo "=== done $(date)" >> $OUT
