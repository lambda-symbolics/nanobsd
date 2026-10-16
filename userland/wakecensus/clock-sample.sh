#!/bin/sh
# clock-sample.sh (on the laptop, /var/tmp/sample.sh): one sample of kernel date,
# raw TSC, PC10 residency, cpu0 hardclock count (cp_time.0 sum) and the raw HPET counter.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
r=$(sysctl -n machdep.cidle.residency | tr ' ' '\n' | awk -F= '/^tsc=/{t=$2} /^pc10=/{p=$2} END{print t, p}')
c=$(sysctl -n kern.cp_time.0 | awk -F'[=,]' '{s=0; for (i=2;i<=NF;i+=2) s+=$i; print s}')
h=$(/var/tmp/memread 0xfed00000 0xf0 | awk '{print $NF}')
echo "$(date +%s) $r $c $((h))"
