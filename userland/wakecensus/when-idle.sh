#!/bin/sh
# when-idle.sh CMD...: wait until the laptop has been untouched for 15 min on
# battery, then run CMD (the measurement scripts).  Started with nohup.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin
while :; do
	idle=$(sysctl -n machdep.lpsched.idle_ms); ac=$(envstat -d acpiacad0 | awk '/connected/{print $2}')
	[ "$idle" -gt 900000 ] && [ "$ac" = FALSE ] && break
	sleep 60
done
echo "=== $(date '+%F %T') idle for $((idle/1000)) s on battery: running $*" >> /var/tmp/when-idle.log
"$@" >> /var/tmp/when-idle.log 2>&1
echo "=== $(date '+%F %T') finished" >> /var/tmp/when-idle.log
