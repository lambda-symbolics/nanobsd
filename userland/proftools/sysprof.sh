#!/bin/sh
# sysprof.sh PID [SECS]: syscall rates of a process by call and by thread (with LWP names)
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/bin:/usr/local/bin
P=$1; S=${2:-3}; F=/var/tmp/sysprof.$P.kt
ps -s -o lid= -o lname= -p $P > /var/tmp/sysprof.$P.names 2>/dev/null
ktrace -t c -f $F -p $P; sleep $S; ktrace -C
kdump -f $F 2>/dev/null | awk '$4 == "CALL" { c = $5; sub(/\(.*/, "", c); print $2, c }' > /var/tmp/sysprof.$P.calls
n=$(wc -l < /var/tmp/sysprof.$P.calls)
echo "total $((n / S))/s"
echo "by call:"; awk '{print $2}' /var/tmp/sysprof.$P.calls | sort | uniq -c | sort -nr | head -10 | awk -v s=$S '{printf "  %7d/s %s\n", $1/s, $2}'
echo "by thread:"; awk '{print $1}' /var/tmp/sysprof.$P.calls | sort | uniq -c | sort -nr | head -10 | while read c l; do
  printf "  %7d/s %-6s %s\n" $((c / S)) $l "$(awk -v l=$l '$1 == l { $1 = ""; print; exit }' /var/tmp/sysprof.$P.names)"; done
echo "by thread x call:"; sort /var/tmp/sysprof.$P.calls | uniq -c | sort -nr | head -12 | while read c l k; do
  printf "  %7d/s %-6s %-22s %s\n" $((c / S)) $l $k "$(awk -v l=$l '$1 == l { $1 = ""; print; exit }' /var/tmp/sysprof.$P.names)"; done
rm -f $F
