#!/bin/sh
# Upload throughput (laptop -> LAN) HT vs legacy, 20 s each.
knob=${KNOB:-hw.iwx.ht}	# KNOB=hw.iwx.vht for the 11ac A/B
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
L=/var/tmp/up-ab.log
exec >$L 2>&1
peer=10.0.0.1
up() {
	for i in $(seq 1 30); do sleep 2; ping -n -c 1 -w 2 $peer >/dev/null 2>&1 && return 0; done
	return 1
}
mode() {
	[ "$(sysctl -n $knob)" = "$1" ] && return 0
	sysctl -w $knob=$1; wpa_cli -i iwx0 reassociate >/dev/null
	up || { sysctl -w $knob=0; wpa_cli -i iwx0 reassociate; up; exit 1; }
	sleep 3
}
measure() {
	dd if=/dev/zero bs=64k 2>/dev/null | nc 192.168.1.242 8766 & P=$!
	sleep 4
	b0=$(netstat -I iwx0 -b | awk 'NR==2{print $6}')
	sleep 20
	b1=$(netstat -I iwx0 -b | awk 'NR==2{print $6}')
	kill $P 2>/dev/null; pkill -x nc; pkill -x dd; wait 2>/dev/null
	echo "$(date +%T) upload $knob=$(sysctl -n $knob): $(( (b1 - b0) * 8 / 20 / 1000 )) kbit/s"
}
for m in 1 0 1; do mode $m; measure; done
sysctl -n hw.iwx.ht_status
echo done
