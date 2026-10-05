#!/bin/sh
# epp-ab.sh [REPS]: energy to finish fixed work at several HWP EPP values,
# on battery, panel off.  Per run: a 60 s idle sample at EPP 240 (package
# C-states and power; also catches an idle state that a load leaves stuck),
# a clean parallel build of wpa_supplicant (gmake -j8), and 60 CPU bursts,
# one every 2 s.  Energy is RAPL package energy plus the battery's charge
# counter; orders rotate between repetitions.  lpschedd is stopped for the
# run (which also turns lpsched packing off) and started again at the end.
# Results: /var/tmp/epp-ab.txt.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin:/usr/local/bin
OUT=/var/tmp/epp-ab.txt
WPA=/var/tmp/wpa211/wpa_supplicant-2.11/wpa_supplicant
BURST=/var/tmp/epp-ab/burst
reps=${1:-3}

rapl() { sysctl -n machdep.cidle.rapl | sed 's/.*raw=\([0-9]*\).*/\1/'; }
joules() { echo "$1 $2" | awk '{ d = $2 - $1; if (d < 0) d += 4294967296; printf "%.1f", d * 61.035 / 1e6 }'; }
charge() { envstat -d acpibat0 | awk '$1 == "charge:" { print $2 }'; }
ac() { envstat -d acpiacad0 | awk '/connected/ { print $2 }'; }
temp() { envstat -d coretemp0 2>/dev/null | awk '/temperature/ { print $3 + 0; exit }'; }
now() { date +%s; }

idle() {	# label: 60 s idle sample
	r0=$(rapl); s0=$(sysctl -n machdep.cidle.residency); t0=$(now)
	sleep 60
	r1=$(rapl); s1=$(sysctl -n machdep.cidle.residency); t1=$(now)
	pc=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {t=v[2,"tsc"]-v[1,"tsc"]; printf "pc2=%.0f pc3=%.0f pc8=%.0f pc10=%.0f", 100*(v[2,"pc2"]-v[1,"pc2"])/t, 100*(v[2,"pc3"]-v[1,"pc3"])/t, 100*(v[2,"pc8"]-v[1,"pc8"])/t, 100*(v[2,"pc10"]-v[1,"pc10"])/t}')
	echo "$(date +%T) idle $1 pkg=$(echo "$(joules $r0 $r1) $((t1 - t0))" | awk '{printf "%.3f", $1/$2}')W $pc temp=$(temp) ac=$(ac)" >> $OUT
}

run() {	# epp
	sysctl -qw machdep.hwp.epp=$1
	(cd $WPA && gmake clean >/dev/null 2>&1)
	sync; sleep 5
	c0=$(charge); r0=$(rapl); t0=$(now)
	(cd $WPA && gmake -j8 wpa_supplicant >/dev/null 2>&1)
	r1=$(rapl); t1=$(now); c1=$(charge)
	echo "$(date +%T) build epp=$1 secs=$((t1 - t0)) pkgJ=$(joules $r0 $r1) batWh=$(echo "$c0 $c1" | awk '{printf "%.3f", $1-$2}') temp=$(temp)" >> $OUT
	sleep 5
	r0=$(rapl); t0=$(now)
	b=$($BURST 60 25000000 2000)
	r1=$(rapl); t1=$(now)
	echo "$(date +%T) burst epp=$1 secs=$((t1 - t0)) pkgJ=$(joules $r0 $r1) $b" >> $OUT
	sysctl -qw machdep.hwp.epp=240
}

echo "=== epp-ab $(date) reps=$reps $(uname -v | awk '{print $4}') ac=$(ac) charge=$(charge)Wh" >> $OUT
/etc/rc.d/lpschedd stop >/dev/null 2>&1
sysctl -qw machdep.hwp.epp=240
i=0
while [ $i -lt $reps ]; do
	case $((i % 3)) in
	0) order="32 128 192 240" ;;
	1) order="240 192 128 32" ;;
	2) order="128 240 32 192" ;;
	esac
	for e in $order; do
		idle "before-epp$e"
		run $e
	done
	i=$((i + 1))
done
idle final
/etc/rc.d/lpschedd start >/dev/null 2>&1
echo "=== done $(date) charge=$(charge)Wh epp=$(sysctl -n machdep.hwp.epp)" >> $OUT
