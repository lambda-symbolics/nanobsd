#!/bin/sh
# A/B download throughput: HT, legacy, HT (20 s each, netstat byte deltas).
knob=${KNOB:-hw.iwx.ht}	# KNOB=hw.iwx.vht for the 11ac A/B
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
L=/var/tmp/ht-ab.log
exec >>$L 2>&1
U=${1:-https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0/amd64/binary/sets/comp.tar.xz}
peer=10.0.0.1

up() {
	for i in $(seq 1 30); do
		sleep 2
		ping -n -c 1 -w 2 $peer >/dev/null 2>&1 && return 0
	done
	return 1
}

mode() {
	[ "$(sysctl -n $knob)" = "$1" ] && return 0
	sysctl -w $knob=$1
	wpa_cli -i iwx0 reassociate >/dev/null
	up || { echo "link down after $knob=$1"; sysctl -w $knob=0;
	    wpa_cli -i iwx0 reassociate; up; exit 1; }
	sleep 3
}

measure() {
	ftp -V -o /dev/null $U >/dev/null 2>&1 &
	P=$!
	sleep 4
	b0=$(netstat -I iwx0 -b | awk 'NR==2{print $5}')
	sleep 20
	b1=$(netstat -I iwx0 -b | awk 'NR==2{print $5}')
	kill $P 2>/dev/null; wait $P 2>/dev/null
	echo "$(date +%T) $knob=$(sysctl -n $knob): $(( (b1 - b0) * 8 / 20 / 1000 )) kbit/s"
}

echo "=== $(date) A/B"
for m in 1 0 1; do
	mode $m
	measure
done
sysctl -n hw.iwx.ht_status
echo "=== done"
