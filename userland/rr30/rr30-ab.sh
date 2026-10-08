#!/bin/sh
# rr30-ab.sh: package power of a still panel at 30 Hz, two ways:
#   drrs : 60 Hz mode selected, kernel idleness DRRS at 30 Hz (cdclk 192 MHz,
#          watermarks for 180 MHz), lispbsd_seamless_rr=1
#   full : 30 Hz mode selected with lispbsd_seamless_rr=2: the pipe configured
#          for the 90.4 MHz pixel clock (cdclk 172.8 MHz, watermarks for it)
# Alternates reps pairs of secs-second windows. Needs Mahogany; the panel is
# powered on directly (the idle module keeps believing it is blanked, so the
# clients stay suspended and nothing draws). env: secs reps
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/bin:/usr/local/sbin
T=/var/tmp/fftest; R=/var/tmp/rr30; OUT=$R/log.txt; secs=${secs:-40}; reps=${reps:-4}
K=hw.drm2.i915_modparams.lispbsd_seamless_rr
mh() { su mag -c "sh $T/mhctl.sh '$1'"; }
out="(first (mahogany::idle-outputs))"
B=0x603c000000
rd() { /var/tmp/memread $(printf "0x%x" $(( B + ($1 & ~0xfff) ))) $(printf "0x%x" $(( $1 & 0xfff ))) | awk '{print $NF}'; }
cdclk() { echo $(( ($(rd 0x46000) & 0x7ff) / 2 + 1 )); }
kv() { echo "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k {print $2}'; }
setrr() {	# mHz: switch, report whether the frame counter restarted (= modeset)
	f0=$(( $(rd 0x70040) )); r=$(mh "(hrt:output-set-refresh $out $1)"); sleep 2; f1=$(( $(rd 0x70040) ))
	echo "  set $1 -> $r; frames $f0 -> $f1 $([ $f1 -lt $f0 ] && echo '(counter restarted: modeset)' || echo '(continuous)')" | tee -a $OUT
}
meas() {	# label
	s0=$(sysctl -n machdep.cidle.residency); e0=$(sysctl -n machdep.cidle.rapl); f0=$(( $(rd 0x70040) )); t0=$(date +%s)
	sleep $secs
	s1=$(sysctl -n machdep.cidle.residency); e1=$(sysctl -n machdep.cidle.rapl); f1=$(( $(rd 0x70040) )); t1=$(date +%s)
	el=$((t1 - t0)); u=$(kv "$e0" uj_per_unit)
	w=$(echo "$(kv "$e0" raw) $(kv "$e1" raw) $el $u" | awk '{d = $2 - $1; if (d < 0) d += 4294967296; printf "pkg=%.2fW", d * $4 / 1e6 / $3}')
	pc=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {t=v[2,"tsc"]-v[1,"tsc"]; printf "pc2=%.0f pc3=%.0f pc6=%.0f pc8=%.0f pc10=%.0f", 100*(v[2,"pc2"]-v[1,"pc2"])/t, 100*(v[2,"pc3"]-v[1,"pc3"])/t, 100*(v[2,"pc6"]-v[1,"pc6"])/t, 100*(v[2,"pc8"]-v[1,"pc8"])/t, 100*(v[2,"pc10"]-v[1,"pc10"])/t}')
	echo "  $(date +%T) [$1] ${el}s $w $pc fps=$(( (f1 - f0) / el )) cdclk=$(cdclk)MHz linkM=$(rd 0x60040) wmL7=$(rd 0x7025c) linetime=$(rd 0x45270) knob=$(sysctl -n $K)" | tee -a $OUT
}
echo "=== rr30-ab $(date) $(uname -v | cut -d: -f1) secs=$secs reps=$reps ac=$(sysctl -n hw.acpi.acpiacad0.connected 2>/dev/null)" | tee -a $OUT
mh "(hrt:output-set-power $out t)" > /dev/null; sleep 3
i=0
while [ $i -lt $reps ]; do
	sysctl -qw $K=1; setrr 60000; sleep 4; meas drrs
	sysctl -qw $K=2; setrr 30000; sleep 4; meas full
	i=$((i + 1))
done
sysctl -qw $K=1; setrr 60000; sleep 2
mh "(when mahogany::*idle-blanked* (hrt:output-set-power $out nil))" > /dev/null
echo "  done, panel off, knob=$(sysctl -n $K)" | tee -a $OUT
