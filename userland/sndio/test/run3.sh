#!/bin/sh
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin
T=/var/tmp/mpvtest; cd $T; rm -f sock mpv3.log
mpv --no-config --vo=null --ao=sndio --really-quiet --msg-level=ao=trace --input-ipc-server=$T/sock --log-file=$T/mpv3.log $T/test.mp4 &
PID=$!
sleep 3
c() { printf '%s\n' "$1" | nc -U -w 2 $T/sock 2>&1 | grep -v '"event"' | head -c 100; }
q() { c '{"command":["get_property","time-pos"]}' | sed 's/.*"data":\([0-9.nul]*\).*/\1/'; }
echo "start t=$(q)"
c '{"command":["set_property","pause",true]}' >/dev/null; sleep 2; echo "paused t=$(q)"
c '{"command":["set_property","pause",false]}' >/dev/null
for i in 1 2 3 4 5 6; do sleep 1; echo "unpause +${i}s t=$(q)"; done
c '{"command":["set_property","pause",true]}' >/dev/null; sleep 1; c '{"command":["set_property","pause",false]}' >/dev/null
for i in 1 2 3 4; do sleep 1; echo "unpause2 +${i}s t=$(q)"; done
c '{"command":["quit"]}' >/dev/null; sleep 2
if kill -0 $PID 2>/dev/null; then echo "STILL ALIVE after quit"; gdb -p $PID -batch -ex 'thread apply all bt' 2>/dev/null | grep -A5 '"ao"' | head -7; kill -9 $PID; fi
wait $PID 2>/dev/null; echo "exit=$?"
echo "=== ao log"; grep '\[ao' mpv3.log | grep -v 'get_state\|^\[.*trace.*delay' | sed -n '1,400p' | grep -v 'starting AO\|Using\|bufsz\|result\|device buffer\|soft-buffer\| - ' | head -40
