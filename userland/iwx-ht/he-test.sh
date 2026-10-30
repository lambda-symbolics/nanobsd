#!/bin/sh
# Dead-man 802.11ax test: switch to an HE network (default net6) with
# hw.iwx.he=1, measure, and always come back to the home network with
# automatic selection restored.  If the WireGuard peer is silent for 90 s
# after a switch, HE goes off again first, then the network is switched back.
#
#   he-test.sh [SSID [HOME_SSID]]        log: /var/tmp/he-test.log
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/bin
SSID=${1:-net6}
HOME_SSID=${2:-Zyxel_7B79_5G}
L=/var/tmp/he-test.log
exec >>$L 2>&1
peer=10.0.0.1

netid() {
	wpa_cli -i iwx0 list_networks |
	    awk -F'\t' -v s="$1" '$2 == s { print $1; exit }'
}

up() {	# up to 90 s for the wg peer
	for i in $(seq 1 45); do
		sleep 2
		ping -n -c 1 -w 2 $peer >/dev/null 2>&1 && return 0
	done
	return 1
}

state() {
	echo "--- $(date) $1"
	wpa_cli -i iwx0 status | grep -E '^(ssid|bssid|freq|wpa_state|ip_address)'
	ifconfig iwx0 | grep -E 'media|status|chan'
	sysctl -n hw.iwx.ht_status
}

go_home() {
	echo "=== $(date) back to $HOME_SSID"
	wpa_cli -i iwx0 select_network "$(netid "$HOME_SSID")"
	up && echo "=== $(date) home link up" ||
	    echo "=== $(date) home link NOT up yet"
	wpa_cli -i iwx0 enable_network all
	state home
}

id=$(netid "$SSID")
[ -n "$id" ] || { echo "no network $SSID"; exit 1; }
echo "=== $(date) start: $SSID (id $id) with hw.iwx.he=1"
state before
sysctl -w hw.iwx.he=1
wpa_cli -i iwx0 select_network "$id"
if up; then
	state "$SSID up"
	echo "--- ping peer 60 s"
	ping -n -c 120 -i 0.5 $peer | tail -3
	echo "--- download 50 MB (internet; Cloudflare answers 403 above 100 MB)"
	curl -s -o /dev/null --max-time 120 -w 'speed %{speed_download} B/s, %{size_download} B, %{time_total} s\n' \
	    'https://speed.cloudflare.com/__down?bytes=50000000'
	echo "--- upload 50 MB (internet)"
	dd if=/dev/urandom bs=1m count=50 2>/dev/null |
	    curl -s -o /dev/null --max-time 120 -w 'speed %{speed_upload} B/s, %{size_upload} B\n' \
	    --data-binary @- 'https://speed.cloudflare.com/__up'
	state "$SSID after transfer"
	echo "--- dmesg"
	dmesg | grep -i iwx | tail -15
else
	echo "=== $(date) no link on $SSID with HE: turning HE off"
	state "$SSID failed"
	dmesg | grep -i iwx | tail -20
	sysctl -w hw.iwx.he=0
fi
go_home
echo "=== $(date) end (hw.iwx.he=$(sysctl -n hw.iwx.he))"
