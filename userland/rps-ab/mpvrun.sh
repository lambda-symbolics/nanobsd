#!/bin/sh
# mpvrun.sh HWDEC: run as mag inside the Mahogany session; fullscreen mpv
# looping the 1440p60 VP9 clip, no audio, IPC on /var/tmp/rps/sock.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin
export XDG_RUNTIME_DIR=/var/run/user/1000 WAYLAND_DISPLAY=wayland-0
rm -f /var/tmp/rps/sock
exec /usr/pkg/bin/mpv --no-config --vo=gpu-next --hwdec=${1:-vaapi} --ao=null \
    --really-quiet --fs --input-ipc-server=/var/tmp/rps/sock --loop=inf \
    /var/tmp/fftest/vp9-1440p60.webm
