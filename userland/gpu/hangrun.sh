#!/bin/sh
# hangrun.sh LABEL SECONDS [mpv options...]: play the Martian in the live
# Wayland session, dump rcs0 registers as soon as the first preemption
# timeout of this run appears, stop at the chip reset or after SECONDS.
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/bin:/usr/local/bin; export PATH
label=$1; secs=$2; shift 2
out=/var/tmp/hangrun-$(date +%H%M%S)-$label; mkdir -p $out
base=$(dmesg | grep -c "preemption time out\|Resetting chip")
export XDG_RUNTIME_DIR=/var/run/user/1000 WAYLAND_DISPLAY=wayland-0
/usr/pkg/bin/mpv --config-dir=/home/mag/.config/mpv --ao=null --fs \
    --msg-level=all=info,vd=v,hwdec=v,vo=v "$@" \
    /home/mag/Desktop/The.Martian.2015.720p.10bit.HDR.BluRay.x265.HEVC-MZABI.mkv \
    > $out/mpv.log 2>&1 &
mpv=$!
t=0; dumped=0; res=nohang
while [ $t -lt $((secs * 5)) ]; do
	sleep 0.2; t=$((t + 1))
	kill -0 $mpv 2>/dev/null || { res=mpv-exited; break; }
	n=$(dmesg | grep -c "preemption time out\|Resetting chip")
	if [ $n -gt $base ] && [ $dumped = 0 ]; then
		dumped=1; res=hang-at-$((t / 5))s
		for i in 1 2 3; do /var/tmp/rcshang > $out/rcs-$i.txt 2>&1; sleep 0.3; done
		ps -axo pid,lid,stat,wchan,comm | grep -E "mpv|mahogany" > $out/ps.txt
	fi
	if dmesg | tail -40 | grep -q "Resetting chip" && [ $dumped = 1 ]; then
		sleep 2; break
	fi
done
kill $mpv 2>/dev/null; sleep 1; kill -9 $mpv 2>/dev/null
dmesg | tail -60 > $out/dmesg-tail.txt
echo "$label: $res ($out)"
