#!/bin/sh
# Dead-man test for hw.iwx.ht: turn 802.11n on and reassociate; keep it
# only if the WireGuard peer answers within 60 s, else turn it off again.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
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

echo "=== $(date) start"
sysctl -n hw.iwx.ht_status
sysctl -w hw.iwx.ht=1
wpa_cli -i iwx0 reassociate
if up; then
	echo "=== $(date) HT up"
else
	echo "=== $(date) no link with HT: back to legacy"
	sysctl -w hw.iwx.ht=0
	wpa_cli -i iwx0 reassociate
	up && echo "=== $(date) legacy up" || echo "=== $(date) legacy down too"
fi
sysctl -n hw.iwx.ht_status
wpa_cli -i iwx0 status | grep -E 'ssid|freq|wpa_state|pairwise|key_mgmt'
ifconfig iwx0 | grep -E 'media|status|channel'
dmesg | tail -15
