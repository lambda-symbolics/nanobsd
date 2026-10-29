#!/bin/sh
# s2stress: hunt the post-resume memory corruption (dumps 21-23: vmem,
# vm_map entries and the gefs block log corrupted 47-155 s after an s2idle
# resume).  Each cycle: timed freeze, then the load that ran before those
# panics, for LOAD seconds: muted audio DMA, mmap/munmap and fork churn,
# gefs writes, VA-API decode on the GPU, network.  Every step is logged and
# synced, so after a panic the log names the cycle and the phase.
#
#   s2stress.sh [CYCLES [FREEZE_S [LOAD_S]]]      (defaults 20 30 180)
#
# Needs AC.  Mutes the outputs first and leaves them muted.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/bin
CYCLES=${1:-20}
FREEZE=${2:-30}
LOAD=${3:-180}
LOG=/var/log/s2stress.log
WORK=/var/tmp/s2stress
CLIP=${CLIP:-/var/tmp/s2stress-clip.mp4}

log() {
	echo "$(date '+%F %T') $*" >> $LOG
	sync
}

mute() {
	for c in $(mixerctl -a 2>/dev/null |
	    sed -n 's/^\(outputs\.[a-z0-9]*\.mute\)=.*/\1/p'); do
		mixerctl -w "$c=on" >/dev/null 2>&1
	done
}

load_audio() {	# continuous playback DMA, inaudible
	while :; do
		audioplay -d /dev/audio0 -f -e slinear_le -P 16 -s 48000 -c 2 \
		    $WORK/silence.raw >/dev/null 2>&1 || sleep 1
	done
}

load_vm() {	# map churn like a media player's buffers, plus fork/exec
	while :; do
		dd if=/dev/zero of=$WORK/map bs=1m count=64 2>/dev/null
		for i in 1 2 3 4 5 6 7 8; do
			md5 $WORK/map >/dev/null
			/bin/sh -c ':'
		done
	done
}

load_fs() {	# gefs block allocation and its log
	n=0
	while :; do
		n=$((n + 1))
		dd if=/dev/urandom of=$WORK/f.$((n % 16)) bs=64k \
		    count=$((n % 64 + 1)) 2>/dev/null
		[ $((n % 8)) = 0 ] && sync
		rm -f $WORK/f.$(((n + 7) % 16))
	done
}

load_gpu() {	# VA-API decode, no window
	[ -f "$CLIP" ] || return 0
	while :; do
		ffmpeg8 -nostdin -loglevel error -hwaccel vaapi \
		    -hwaccel_device /dev/dri/renderD128 -i "$CLIP" -f null - \
		    >/dev/null 2>&1 || sleep 2
	done
}

load_net() {
	while :; do
		ping -n -c 5 -i 0.2 10.0.0.1 >/dev/null 2>&1
		sleep 1
	done
}

stop_loads() {
	for p in $PIDS; do
		pkill -P $p 2>/dev/null
		kill $p 2>/dev/null
	done
	pkill -f "audioplay -d /dev/audio0 -f -e slinear_le" 2>/dev/null
	pkill -f "ffmpeg8 -nostdin -loglevel error -hwaccel vaapi" 2>/dev/null
	PIDS=
}

[ "$(envstat -d acpiacad0 2>/dev/null | awk '/connected/ {print $2}')" = TRUE ] ||
	{ echo "s2stress: not on AC" >&2; exit 1; }
mkdir -p $WORK
[ -f $WORK/silence.raw ] ||
	dd if=/dev/zero of=$WORK/silence.raw bs=192000 count=10 2>/dev/null
mute
trap 'stop_loads; log "stopped"; exit 0' INT TERM
log "start: $CYCLES cycles, freeze ${FREEZE}s, load ${LOAD}s, $(uname -v | sed 's/:.*//')"
oldto=$(sysctl -n hw.acpi.sleep.freeze_timeout)
i=1
while [ $i -le $CYCLES ]; do
	log "cycle $i: freeze"
	sysctl -w hw.acpi.sleep.freeze_timeout=$FREEZE >/dev/null
	/usr/local/bin/s0ix suspend
	sysctl -w hw.acpi.sleep.freeze_timeout=$oldto >/dev/null
	log "cycle $i: resumed, load"
	mute
	PIDS=
	load_audio & PIDS="$PIDS $!"
	load_vm & PIDS="$PIDS $!"
	load_fs & PIDS="$PIDS $!"
	load_gpu & PIDS="$PIDS $!"
	load_net & PIDS="$PIDS $!"
	t=0
	while [ $t -lt $LOAD ]; do
		sleep 15
		t=$((t + 15))
		log "cycle $i: +${t}s ok"
	done
	stop_loads
	i=$((i + 1))
done
log "done: $CYCLES cycles without a panic"
