#!/bin/sh
# rr30-ab6.sh: still panel, four pipe configurations cycled, secs each, reps rounds:
#   A drrs    knob=1, 60 Hz mode, kernel idleness DRRS at 30 Hz (cdclk 192)
#   B seam30  knob=1, explicit seamless 30 Hz mode (reading mode state, cdclk 192)
#   C full30  knob=2, 30 Hz mode, pipe configured for 90.4 MHz (cdclk 172.8)
#   D full60  knob=2, 60 Hz mode, no seamless flags, idleness DRRS (cdclk 192)
# The idle module is poked (deadlines from now) and its blank delay raised for the run.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/bin:/usr/local/sbin
T=/var/tmp/fftest; R=/var/tmp/rr30; OUT=$R/log.txt; secs=${secs:-30}; reps=${reps:-3}
K=hw.drm2.i915_modparams.lispbsd_seamless_rr
mh() { su mag -c "sh $T/mhctl.sh '$1'"; }
out="(first (mahogany::idle-outputs))"
B=0x603c000000
rd() { /var/tmp/memread $(printf "0x%x" $(( B + ($1 & ~0xfff) ))) $(printf "0x%x" $(( $1 & 0xfff ))) | awk '{print $NF}'; }
cdclk() { echo $(( ($(rd 0x46000) & 0x7ff) / 2 + 1 )); }
kv() { echo "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k {print $2}'; }
setrr() { f0=$(( $(rd 0x70040) )); r=$(mh "(hrt:output-set-refresh $out $1)"); sleep 2; f1=$(( $(rd 0x70040) )); echo "  set $1 -> $r; frames $f0 -> $f1" | tee -a $OUT; }
meas() {
	s0=$(sysctl -n machdep.cidle.residency); e0=$(sysctl -n machdep.cidle.rapl); f0=$(( $(rd 0x70040) )); g0=$(printf "%d" $(rd 0x138108)); t0=$(date +%s)
	sleep $secs
	s1=$(sysctl -n machdep.cidle.residency); e1=$(sysctl -n machdep.cidle.rapl); f1=$(( $(rd 0x70040) )); g1=$(printf "%d" $(rd 0x138108)); t1=$(date +%s)
	el=$((t1 - t0)); u=$(kv "$e0" uj_per_unit)
	w=$(echo "$(kv "$e0" raw) $(kv "$e1" raw) $(kv "$e0" pp1) $(kv "$e1" pp1) $el $u" | awk '{d = $2 - $1; if (d < 0) d += 4294967296; g = $4 - $3; if (g < 0) g += 4294967296; printf "pkg=%.2fW gpu=%.2fW", d * $6 / 1e6 / $5, g * $6 / 1e6 / $5}')
	pc=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {t=v[2,"tsc"]-v[1,"tsc"]; printf "pc2=%.0f pc3=%.0f pc6=%.0f pc8=%.0f pc10=%.0f", 100*(v[2,"pc2"]-v[1,"pc2"])/t, 100*(v[2,"pc3"]-v[1,"pc3"])/t, 100*(v[2,"pc6"]-v[1,"pc6"])/t, 100*(v[2,"pc8"]-v[1,"pc8"])/t, 100*(v[2,"pc10"]-v[1,"pc10"])/t}')
	r=$(echo "$g0 $g1 $el" | awk '{d=$2-$1; if (d<0) d+=4294967296; printf "%.0f%%", 100*d*1.28e-6/$3}')
	echo "  $(date +%T) [$1] ${el}s $w $pc rc6=$r fps=$(( (f1 - f0) / el )) cdclk=$(cdclk)MHz linkM=$(rd 0x60040) wmL7=$(rd 0x7025c) linetime=$(rd 0x45270) pipeconf=$(rd 0x70008) knob=$(sysctl -n $K)" | tee -a $OUT
}
echo "=== rr30-ab6 $(date) $(uname -v | cut -d: -f1) secs=$secs reps=$reps" | tee -a $OUT
mh "(progn (setf mahogany::*idle-dim-seconds* nil mahogany::*idle-blank-seconds* 1800) (mahogany::idle-poke))" > /dev/null; sleep 4
i=0
# battery ground truth: charge (Wh, 1 mWh steps) before and after each window
wh() { envstat -d acpibat0 | awk '/ charge:/ {print $2}'; }
measb() {	# label: meas plus the battery's own average discharge
	c0=$(wh); b0=$(date +%s); meas "$1"; c1=$(wh); b1=$(date +%s)
	echo "    battery: $(echo "$c0 $c1 $b0 $b1" | awk '{printf "%.2f W over %d s (%.3f -> %.3f Wh)", ($1-$2)*3600/($4-$3), $4-$3, $1, $2}') rate_now=$(envstat -d acpibat0 | awk '/discharge rate:/ {print $3}')W" | tee -a $OUT
}
# A knob=1 60 Hz mode (idle DRRS 30 Hz, fast watermarks); E knob=3 explicit 30 Hz, slow watermarks
while [ $i -lt $reps ]; do
	sysctl -qw $K=1; setrr 30000; setrr 60000; sleep 4; measb A-drrs
	sysctl -qw $K=3; setrr 30000; sleep 4; measb E-seam30-slowwm
	i=$((i + 1))
done
sysctl -qw $K=3; setrr 60000; sleep 2
mh "(progn (setf mahogany::*idle-dim-seconds* 120 mahogany::*idle-blank-seconds* 300) (mahogany::idle-blank) (mahogany::idle-schedule))" > /dev/null
sleep 5; measb panel-off
echo "  done, panel off, knob=$(sysctl -n $K)" | tee -a $OUT
