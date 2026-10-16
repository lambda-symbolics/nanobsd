#!/bin/sh
# uapsd-ab.sh [minutes] [rounds]: whole-machine battery power with
# hw.iwx.uapsd=0 and =1, alternating (0 1 0 1 ... for ROUNDS rounds), from the
# battery's own charge counter (envstat acpibat0 "charge", 1 mWh steps), plus
# iwx interrupts and a sparse ping to the default gateway after each window.
#
# Run it ON THE LAPTOP, detached, and leave the machine alone:
#   nohup sh /var/tmp/uapsd-ab.sh 6 2 > /dev/null 2>&1 &
# Preconditions: on battery, >= 10 minutes after boot, panel blanked (not the
# lid: the lid suspends), associated to the home AP, `ifconfig iwx0 powersave`.
# Each switch reassociates (wpa_cli reassociate, which may scan once).
# Log: /var/tmp/uapsd-ab/log.txt.  Leaves hw.iwx.uapsd=0 and reassociates.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin
m=${1:-6}; rounds=${2:-2}; settle=${settle:-120}; IF=${IF:-iwx0}
D=/var/tmp/uapsd-ab; OUT=$D/log.txt
mkdir -p $D

log() { echo "$(date +%T) $*" | tee -a $OUT; }
wh() { envstat -d acpibat0 | awk '/ charge:/ {print $2}'; }
# iwx0's interrupt: msix3 on the Nano (docs/rpm-s0ix-findings.org); check with
# `intrctl list | grep iwx0` and override with IRQPAT='msix3 vec' if it moved.
IRQPAT=${IRQPAT:-msix3 vec}
irq() { vmstat -i | awk -v p="$IRQPAT" 'index($0, p) == 1 {s += $(NF-1)} END {print s + 0}'; }
gw() { route -n get default 2>/dev/null | awk '/gateway:/ {print $2}'; }
field() { echo "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k {print $2}'; }

assoc_wait() {	# up to 90 s for wpa_supplicant COMPLETED and the gateway answering
	i=0
	while [ $i -lt 90 ]; do
		if wpa_cli -i $IF status 2>/dev/null | grep -q '^wpa_state=COMPLETED' &&
		    ping -n -c 1 -w 2 $GW > /dev/null 2>&1; then
			return 0
		fi
		sleep 1; i=$((i + 1))
	done
	return 1
}

setmode() {	# 0|1: set the switch, reassociate, wait, settle
	sysctl -w hw.iwx.uapsd=$1 > /dev/null
	wpa_cli -i $IF reassociate > /dev/null
	sleep 5
	if ! assoc_wait; then
		log "uapsd=$1: no association after 90 s"
		return 1
	fi
	sleep $settle
	ST=$(sysctl -n hw.iwx.uapsd_status)
	log "uapsd=$1: $ST"
	if [ "$1" = 1 ] && [ "$(field "$ST" negotiated)" != 1 ]; then
		log "uapsd=1 but U-APSD not negotiated (see ap_wmm/ap_uapsd/qos above)"
	fi
	return 0
}

measure() {	# mode
	c0=$(wh); q0=$(irq); t0=$(date +%s)
	sleep $((m * 60))
	c1=$(wh); q1=$(irq); t1=$(date +%s)
	log "uapsd=$1 RESULT $(echo "$c0 $c1 $t0 $t1 $q0 $q1" | awk '{
	    printf "battery %.3f W over %d s (%.3f -> %.3f Wh), iwx irq %.2f/s",
	    ($1 - $2) * 3600 / ($4 - $3), $4 - $3, $1, $2, ($6 - $5) / ($4 - $3)}')"
	log "uapsd=$1 status after: $(sysctl -n hw.iwx.uapsd_status)"
	log "uapsd=$1 ping $GW: $(ping -n -c 30 -i 1 $GW 2>&1 | tail -2 | tr '\n' ' ')"
}

restore() {
	sysctl -w hw.iwx.uapsd=0 > /dev/null
	wpa_cli -i $IF reassociate > /dev/null
}

if envstat -d acpiacad0 | grep -q 'connected: *TRUE'; then
	echo "uapsd-ab: on AC; unplug the charger first" >&2
	exit 1
fi
GW=$(gw)
[ -n "$GW" ] || { echo "uapsd-ab: no default gateway" >&2; exit 1; }
trap 'restore; log "interrupted, hw.iwx.uapsd=0"; exit 1' INT TERM

log "=== uapsd-ab $(uname -v | cut -d: -f1) minutes=$m rounds=$rounds settle=${settle}s gw=$GW up:$(uptime)"
r=0
while [ $r -lt $rounds ]; do
	for mode in 0 1; do
		setmode $mode && measure $mode
	done
	r=$((r + 1))
done
restore
log "done, hw.iwx.uapsd=0 (reassociated)"
grep RESULT $OUT | tail -$((rounds * 2))
