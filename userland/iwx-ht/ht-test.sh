#!/bin/sh
# Dead-man test for hw.iwx.ht (default) or hw.iwx.vht ("ht-test.sh vht"):
# turn it on and reassociate; keep it only if the WireGuard peer answers
# within 60 s, else turn it off again.  Log: /var/tmp/ht-test.log
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
knob=hw.iwx.${1:-ht}
L=/var/tmp/ht-test.log
exec >>$L 2>&1
peer=10.0.0.1

up() {
	for i in $(seq 1 30); do
		sleep 2
		ping -n -c 1 -w 2 $peer >/dev/null 2>&1 && return 0
	done
	return 1
}

echo "=== $(date) start $knob"
sysctl -n hw.iwx.ht_status
sysctl -w $knob=1
wpa_cli -i iwx0 reassociate
if up; then
	echo "=== $(date) $knob up"
else
	echo "=== $(date) no link with $knob: turning it off again"
	sysctl -w $knob=0
	wpa_cli -i iwx0 reassociate
	up && echo "=== $(date) link up without $knob" ||
	    echo "=== $(date) link down without $knob too"
fi
sysctl -n hw.iwx.ht_status
wpa_cli -i iwx0 status | grep -E 'ssid|freq|wpa_state|pairwise|key_mgmt'
ifconfig iwx0 | grep -E 'media|status|channel'
dmesg | tail -15
