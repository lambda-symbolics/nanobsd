#!/bin/sh
# refresh-ab.sh [REPS]: Firefox (Wayland) showing anim.html, panel on, the
# refresh policy stopped; package power with the panel forced to 60 Hz and
# to 30 Hz, interleaved, plus a still page, the page in another group and
# with the panel off.  Compositor frame rates from Mahogany's frame counter.
# Results in /var/tmp/pmeas.txt.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin
T=/var/tmp/fftest; reps=${1:-3}
# mag's login shell is cclsh: no VAR=value prefixes, so go through sh.
mh() { su mag -c "sh $T/mhctl.sh '$1'"; }
rate() { mh "(hrt:output-set-refresh (first (mahogany::idle-outputs)) $1)" > /dev/null; }
frames() { mh "(hrt:output-frames-rendered (first (mahogany::idle-outputs)))"; }
B=0x603c000000	# i915 MMIO BAR
rd() { /var/tmp/memread $(printf "0x%x" $(( B + ($1 & ~0xfff) ))) $(printf "0x%x" $(( $1 & 0xfff ))) | awk '{print $NF}'; }
rc6() { printf "%d" $(rd 0x138108); }	# GT RC6 residency, 1.28 us units
meas() {	# label
	f0=$(frames); s0=$(sysctl -n machdep.cidle.residency); g0=$(rc6); t0=$(date +%s)
	/var/tmp/pmeas.sh "$1" 60 > /dev/null
	f1=$(frames); s1=$(sysctl -n machdep.cidle.residency); g1=$(rc6); t1=$(date +%s)
	pc=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[NR,a[1]]=a[2]}}
	    END {t=v[2,"tsc"]-v[1,"tsc"]; printf "pc2=%.0f pc3=%.0f pc8=%.0f pc10=%.0f", 100*(v[2,"pc2"]-v[1,"pc2"])/t, 100*(v[2,"pc3"]-v[1,"pc3"])/t, 100*(v[2,"pc8"]-v[1,"pc8"])/t, 100*(v[2,"pc10"]-v[1,"pc10"])/t}')
	echo "  $1 fps=$(echo "$f0 $f1 $t0 $t1" | awk '{printf "%.1f", ($2-$1)/($4-$3)}') $pc rc6=$(echo "$g0 $g1 $t0 $t1" | awk '{d=$2-$1; if (d<0) d+=4294967296; printf "%.0f%%", 100*d*1.28e-6/($4-$3)}')" >> /var/tmp/pmeas.txt
}
echo "=== refresh-ab $(date) ac=$(envstat -d acpiacad0 | awk '/connected/ {print $2}')" >> /var/tmp/pmeas.txt
/etc/rc.d/lpschedd stop > /dev/null 2>&1	# fixed EPP for every condition
sysctl -qw machdep.hwp.epp=240
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null
mh "(mahogany::refresh-stop)" > /dev/null
su mag -c "sh $T/mhgrp.sh 6" > /dev/null; sleep 2
meas desktop-60
su mag -c "sh $T/launch.sh wayland file://$T/anim.html"; sleep 25
echo "  gl: $(sh $T/gl.sh | tr '\n' ' ')" >> /var/tmp/pmeas.txt
i=0
while [ $i -lt $reps ]; do
	for r in 60000 30000; do rate $r; sleep 10; meas anim-$((r / 1000))hz; done
	i=$((i + 1))
done
rate 60000
su mag -c "sh $T/mhgrp.sh 5" > /dev/null; sleep 10; meas anim-other-group
su mag -c "sh $T/mhgrp.sh 6" > /dev/null; sleep 5
su mag -c "sh $T/mhform.sh $T/mh-off.lisp" > /dev/null; sleep 10; meas anim-panel-off
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null; sleep 3
sh $T/ffkill.sh; sleep 5
su mag -c "sh $T/launch.sh wayland file://$T/anim.html?static"; sleep 25
meas still-page
su mag -c "sh $T/mhform.sh $T/mh-off.lisp" > /dev/null; sleep 10; meas still-page-panel-off
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null; sleep 3
sh $T/ffkill.sh; sleep 5
su mag -c "sh $T/mhform.sh $T/mh-off.lisp" > /dev/null; sleep 10; meas no-firefox-panel-off
su mag -c "sh $T/mhform.sh $T/mh-on.lisp" > /dev/null; sleep 3
mh "(mahogany::refresh-start)" > /dev/null
su mag -c "sh $T/mhform.sh $T/mh-restore.lisp" > /dev/null
su mag -c "sh $T/mhform.sh $T/mh-off.lisp" > /dev/null
/etc/rc.d/lpschedd start > /dev/null 2>&1
echo "=== refresh-ab done $(date)" >> /var/tmp/pmeas.txt
