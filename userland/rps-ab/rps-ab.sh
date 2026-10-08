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
mh() { su mag -c "sh $T/mhctl.sh '$1'"; }
frames() { mh "(hrt:output-frames-rendered (first (mahogany::idle-outputs)))"; }
B=0x603c000000
rd() { /var/tmp/memread $(printf "0x%x" $(( B + ($1 & ~0xfff) ))) $(printf "0x%x" $(( $1 & 0xfff ))) | awk '{print $NF}'; }
rc6() { printf "%d" $(rd 0x138108); }
rapl() { sysctl -n machdep.cidle.rapl; }
kv() { echo "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k {print $2}'; }
mpvq() { printf '{ "command": ["get_property", "%s"] }\n' "$1" | nc -U -w 2 $R/sock 2>/dev/null | sed 's/.*"data":\([^,]*\),.*/\1/'; }
policy() {	# name; kernel 137 knobs: timer, park_down, unpark_start, max/min/boost
	sysctl -qw hw.i915rps.unpark_start=0 hw.i915rps.max_mhz=1100 hw.i915rps.boost_mhz=1100 hw.i915rps.park_down=0
	case $1 in
	stock)	# PM interrupts (never fire here) and cur_freq left where the last boost put it: force 1100
		sysctl -qw hw.i915rps.timer=0 hw.i915rps.min_mhz=1100 hw.i915rps.min_mhz=100 ;;
	rpe)	sysctl -qw hw.i915rps.timer=0 hw.i915rps.unpark_start=1 ;;
	timer)	sysctl -qw hw.i915rps.timer=1 hw.i915rps.park_down=1 ;;
	timer-nopd) sysctl -qw hw.i915rps.timer=1 hw.i915rps.park_down=0 ;;
	cap*)	sysctl -qw hw.i915rps.timer=1 hw.i915rps.park_down=1 hw.i915rps.max_mhz=${1#cap} ;;
	esac
}
meas() {	# label
	f0=$(frames); s0=$(sysctl -n machdep.cidle.residency); g0=$(rc6); e0=$(rapl)
	st0=$(sysctl -n hw.i915rps.stats); d0=$(mpvq frame-drop-count); t0=$(date +%s)
	: > $R/act.txt
	( end=$(( $(date +%s) + secs )); while [ $(date +%s) -lt $end ]; do sysctl -n hw.i915rps.act_mhz >> $R/act.txt; sleep 2; done ) &
	sampler=$!
	wait $sampler
	f1=$(frames); s1=$(sysctl -n machdep.cidle.residency); g1=$(rc6); e1=$(rapl)
	st1=$(sysctl -n hw.i915rps.stats); d1=$(mpvq frame-drop-count); t1=$(date +%s)
	el=$((t1 - t0))
	u=$(kv "$e0" uj_per_unit)
	w=$(echo "$(kv "$e0" raw) $(kv "$e1" raw) $(kv "$e0" pp1) $(kv "$e1" pp1) $el $u" | awk '{
		d = $2 - $1; if (d < 0) d += 4294967296; g = $4 - $3; if (g < 0) g += 4294967296;
		printf "pkg=%.2fW gpu=%.2fW", d * $6 / 1e6 / $5, g * $6 / 1e6 / $5 }')
	pc=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {t=v[2,"tsc"]-v[1,"tsc"]; printf "pc2=%.0f pc8=%.0f pc10=%.0f", 100*(v[2,"pc2"]-v[1,"pc2"])/t, 100*(v[2,"pc8"]-v[1,"pc8"])/t, 100*(v[2,"pc10"]-v[1,"pc10"])/t}')
	act=$(awk '$1 > 0 {n++; s += $1; if ($1 > m) m = $1} END {if (n) printf "act=%.0fMHz(n=%d max=%d)", s/n, n, m; else print "act=parked"}' $R/act.txt)
	ev=$(printf '%s\n%s\n' "$st0" "$st1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {printf "irq=%d up=%d down=%d timeout=%d boost=%d park=%d tick=%d", v[2,"irq"]-v[1,"irq"], v[2,"up"]-v[1,"up"], v[2,"down"]-v[1,"down"], v[2,"timeout"]-v[1,"timeout"], v[2,"boost"]-v[1,"boost"], v[2,"park"]-v[1,"park"], v[2,"tick"]-v[1,"tick"]}')
	fps=$(echo "$f0 $f1 $el" | awk '{printf "%.1f", ($2-$1)/$3}')
	r=$(echo "$g0 $g1 $el" | awk '{d=$2-$1; if (d<0) d+=4294967296; printf "%.0f%%", 100*d*1.28e-6/$3}')
	echo "  $(date +%T) [$1] ${el}s $w $pc rc6=$r $act $ev fps=$fps drops=$((d1 - d0)) cur=$(sysctl -n hw.i915rps.cur_mhz)" | tee -a $OUT
}
mpvstart() { su mag -c "sh $R/mpvrun.sh $1" > /dev/null 2>&1 & sleep 8; }
mpvstop() { printf '{ "command": ["quit"] }\n' | nc -U -w 2 $R/sock > /dev/null 2>&1; sleep 3; pkill -x mpv; sleep 2; }
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
