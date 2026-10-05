#!/bin/sh
# launch.sh x|wayland: start the test Firefox in the running session
cd /var/tmp/fftest
if [ "$1" = wayland ]; then
	export XDG_RUNTIME_DIR=/var/run/user/1000 WAYLAND_DISPLAY=wayland-0 MOZ_ENABLE_WAYLAND=1
	unset DISPLAY
else
	export DISPLAY=:0 XAUTHORITY=/home/mag/.Xauthority MOZ_ENABLE_WAYLAND=0
fi
nohup /usr/pkg/bin/firefox --no-remote --profile /var/tmp/fftest/prof "${2:-file:///var/tmp/fftest/play.html}" > /var/tmp/fftest/ff-$1.log 2>&1 &
echo $! > /var/tmp/fftest/ff.pid
