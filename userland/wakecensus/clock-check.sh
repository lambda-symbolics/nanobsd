#!/bin/bash
# clock-check.sh: run on the workstation.  True time = this machine's NTP clock;
# the laptop is sampled over ssh (clock-sample.sh installed as /var/tmp/sample.sh).
# Usage: clock-check.sh [host]   (measure after 10 min of uptime: PC8 boot transient)
H=${1:-root@192.168.1.254}
S() { local a b out; a=$(date +%s.%N); out=$(timeout 20 ssh -o ConnectTimeout=8 $H sh /var/tmp/sample.sh 2>/dev/null); b=$(date +%s.%N); echo "$(echo "($a+$b)/2" | bc -l) $out"; }
cfg() { timeout 20 ssh $H "/sbin/sysctl -w $* >/dev/null" 2>/dev/null; }
win() {
  local s0 s1; s0=$(S); local t=$(date +%s); until [ $(( $(date +%s) - t )) -ge $2 ]; do sleep 5; done; s1=$(S)
  set -- "$1" $s0 $s1
  awk -v l="$1" -v w0=$2 -v d0=$3 -v t0=$4 -v p0=$5 -v c0=$6 -v h0=$7 -v w1=$8 -v d1=$9 -v t1=${10} -v p1=${11} -v c1=${12} -v h1=${13} 'BEGIN{
    real=w1-w0; tsc=(t1-t0)/2112000000; hd=h1-h0; if (hd<0) hd+=4294967296; hpet=hd/19200000;
    printf "%-36s real %6.1f s | kernel date %5d | TSC %6.1f | HPET %6.1f | cpu0 ticks %6.1f s | PC10 %4.1f%%\n", l, real, d1-d0, tsc, hpet, (c1-c0)/100, 100*(p1-p0)/(t1-t0) }'
}
cfg machdep.cidle.tickless=1 machdep.cidle.maxskip=100 machdep.cidle.minskip=4 machdep.cidle.hint=0x60
win "tickless, maxskip 100" 120
cfg machdep.cidle.maxskip=300
win "tickless, maxskip 300" 120
cfg kern.timecounter.hardware=hpet0
win "maxskip 300, timecounter hpet0" 90
cfg kern.timecounter.hardware=TSC
win "maxskip 300, back to TSC" 90
cfg machdep.cidle.maxskip=100
echo done
