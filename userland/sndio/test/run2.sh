#!/bin/sh
# Headless mpv tests for the libsndio fixes (docs/mpv-sndio.org): a silent
# black clip under /var/tmp/mpvtest (see the ffmpeg8 lines in the doc), no
# window, commands over the IPC socket, time-pos polled after each.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin
T=/var/tmp/mpvtest; cd $T; rm -f sock mpv2.log
mpv --no-config --vo=null --ao=sndio --really-quiet --input-ipc-server=$T/sock --log-file=$T/mpv2.log --sub-file=a.srt --sub-file=b.srt $T/test2.mp4 &
PID=$!
sleep 3
c() { printf '%s\n' "$1" | nc -U -w 2 $T/sock 2>&1 | grep -v '"event"' | head -c 100; }
q() { c '{"command":["get_property","time-pos"]}' | sed 's/.*"data":\([0-9.nul]*\).*/\1/'; }
step() { echo "--- $1"; c "$2" >/dev/null; sleep 1.5; a=$(q); sleep 1.5; b=$(q); echo "    t=$a -> $b $( [ "$a" != "$b" ] && echo ADVANCING || echo STUCK )"; }
echo "start t=$(q)"
step "seek 40"        '{"command":["seek","40","absolute"]}'
step "pause"          '{"command":["set_property","pause",true]}'
c '{"command":["set_property","pause",false]}' >/dev/null; sleep 1.5; a=$(q); sleep 1.5; b=$(q); echo "--- unpause\n    t=$a -> $b $( [ "$a" != "$b" ] && echo ADVANCING || echo STUCK )"
step "audio track 2"  '{"command":["set_property","aid",2]}'
step "audio track 1"  '{"command":["set_property","aid",1]}'
step "sub track 2"    '{"command":["set_property","sid",2]}'
step "sub track off"  '{"command":["set_property","sid","no"]}'
step "seek 10"        '{"command":["seek","10","absolute"]}'
step "seek -5 rel"    '{"command":["seek","-5","relative"]}'
echo "aid=$(c '{"command":["get_property","aid"]}') sid=$(c '{"command":["get_property","sid"]}')"
c '{"command":["quit"]}' >/dev/null; sleep 2
if kill -0 $PID 2>/dev/null; then echo "STILL ALIVE after quit"; gdb -p $PID -batch -ex 'thread apply all bt' 2>/dev/null | grep -A6 '"ao"' | head -8; kill -9 $PID; fi
wait $PID 2>/dev/null; echo "exit=$?"
grep -i 'partial\|error\|sio_' mpv2.log | head
