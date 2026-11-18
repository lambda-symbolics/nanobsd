#!/bin/sh
# sndsusp.sh: tone playing across a 20 s s2idle; logs to /var/tmp/sndsusp/
D=/var/tmp/sndsusp; mkdir -p $D; cd $D
S=/sbin/sysctl
[ -f tone90.raw ] || python3.13 -c "
import math,struct
f=open('tone90.raw','wb')
fr=bytearray()
for i in range(48000*90):
    v=int(8000*math.sin(2*math.pi*440*i/48000)); fr+=struct.pack('<hh',v,v)
f.write(fr)"
mixerctl -n outputs.master > master.saved
mixerctl -w outputs.master=0 >/dev/null
codec() { for n in 2 3; do echo "nid $n fmt $(/var/tmp/hdaverb 0 $n 0xa00 0) strm $(/var/tmp/hdaverb 0 $n 0xf06 0)"; done; }
snap() { echo "== $1 $(date +%T) irqs=$($S -n hw.hdaudio.stats.stream_irqs) idle_stops=$($S -n hw.audio0.idle_stops) pmcsr=$(/usr/sbin/pcictl pci0 read -d 31 -f 3 0x54)"
  /var/tmp/hdaregs 7 2>&1 | head -3; codec
  ps -o pid,lstate,wchan,etime,args -p $AP | tail -1; }
/usr/bin/audioplay -d /dev/audio0 -f -e slinear_le -P 16 -s 48000 -c 2 tone90.raw > ap.out 2>&1 &
AP=$!
sleep 3
{ snap before; /var/tmp/ringcap 1 >/dev/null 2>&1; cp /var/tmp/ring.raw ring.before
  $S -w hw.acpi.sleep.freeze_timeout=20 >/dev/null
  $S -w hw.acpi.sleep.freeze=1
  $S -w hw.acpi.sleep.freeze_timeout=0 >/dev/null
  sleep 2; snap after2s; /var/tmp/ringcap 1 >/dev/null 2>&1; cp /var/tmp/ring.raw ring.after
  sleep 5; snap after7s
} > log 2>&1
wait $AP; echo "audioplay exit $? $(date +%T)" >> log
mixerctl -w outputs.master=$(cat master.saved) >/dev/null
echo done >> log
