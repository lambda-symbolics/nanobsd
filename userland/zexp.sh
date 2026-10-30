#!/bin/sh
# Stray-zero-write experiments (docs/post-resume-corruption.org).
#   zexp.sh prep | A | B | C | D | E | F      log: /var/log/zexp.log
# memwatch (userland/memwatch.c) is expected at /var/tmp/memwatch.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/bin:/usr/local/bin:/usr/local/sbin
L=/var/log/zexp.log
MB=${MB:-3500}
log() { echo "$(date '+%F %T') $*" >> $L; sync; }
run() { log "memwatch $MB MB, $1 rounds, trigger: $2"; /var/tmp/memwatch $MB "$1" "$2" >> $L 2>&1; sync; }
case "$1" in
prep)
	for c in $(mixerctl -a | sed -n 's/^\(outputs\.[a-z0-9]*\.mute\)=.*/\1/p'); do
		mixerctl -w "$c=on" >/dev/null; done
	log "muted: $(mixerctl -a | grep 'outputs\..*mute' | tr '\n' ' ')"
	/usr/local/bin/brightness dim 1 >/dev/null 2>&1; log "panel dimmed"
	gefs snap / main pre-zexp-$(date +%Y%m%d-%H%M) && log "gefs snapshot taken"
	;;
A)	run 3 '' ;;
B)	run ${ROUNDS:-10} '/usr/local/bin/usb on usbc >/dev/null 2>&1; sleep 2; /usr/local/bin/usb off usbc >/dev/null 2>&1' ;;
C)	run ${ROUNDS:-10} 'sysctl -w hw.acpi.sleep.tcss_d3=0 >/dev/null; sleep 2; sysctl -w hw.acpi.sleep.tcss_d3=1 >/dev/null' ;;
D)	old=$(sysctl -n hw.acpi.sleep.freeze_timeout); run ${ROUNDS:-5} 'sysctl -w hw.acpi.sleep.freeze_timeout=20 >/dev/null; /usr/local/bin/s0ix suspend'; sysctl -w hw.acpi.sleep.freeze_timeout=$old >/dev/null; log "freeze_timeout back to $old" ;;
E)	run ${ROUNDS:-10} '/usr/local/bin/bluetooth on >/dev/null 2>&1; sleep 4; btconfig ubt0 up >/dev/null 2>&1; sleep 6; btconfig ubt0 down >/dev/null 2>&1; /usr/local/bin/usb off >/dev/null 2>&1' ;;
F)	/usr/local/bin/bluetooth on >/dev/null 2>&1; sleep 4; btconfig ubt0 up; log "usb on + bt up: $(/usr/local/bin/usb status 2>&1)"; old=$(sysctl -n hw.acpi.sleep.freeze_timeout); run ${ROUNDS:-5} 'sysctl -w hw.acpi.sleep.freeze_timeout=20 >/dev/null; /usr/local/bin/s0ix suspend'; sysctl -w hw.acpi.sleep.freeze_timeout=$old >/dev/null; log "freeze_timeout back to $old" ;;
*)	echo "usage: zexp.sh prep|A|B|C|D|E|F" >&2; exit 2 ;;
esac
