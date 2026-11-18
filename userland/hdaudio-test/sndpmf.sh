#!/bin/sh
# sndpmf.sh: tone at volume 0, audio0 suspended and resumed through pmf
# (drvctl), then the hardware ring is checked for the tone.
D=/var/tmp/sndsusp; cd $D
mixerctl -n outputs.master > master.saved
mixerctl -w outputs.master=0 >/dev/null
/usr/bin/audioplay -d /dev/audio0 -f -e slinear_le -P 16 -s 48000 -c 2 tone90.raw > ap.out 2>&1 &
AP=$!
sleep 2
/var/tmp/ringcap 1 >/dev/null 2>&1; cp /var/tmp/ring.raw pmf.before
/sbin/drvctl -S audio0; echo "suspend $?"; sleep 1; /sbin/drvctl -R audio0; echo "resume $?"
sleep 1
/var/tmp/ringcap 1 >/dev/null 2>&1; cp /var/tmp/ring.raw pmf.after
ps -o lstate,wchan -p $AP | tail -1
# On a kernel with the bug the close drains a skipped track: do not wait.
kill -9 $AP
mixerctl -w outputs.master=$(cat master.saved) >/dev/null
echo "master now $(mixerctl -n outputs.master)"
python3.13 - <<'P'
import array
for fn in ('pmf.before','pmf.after'):
    d=array.array('h',open(fn,'rb').read()); L=d[0::2]
    n=len(L); peak=max(abs(x) for x in L)
    jumps=sum(1 for i in range(2,n) if abs(L[i]-2*L[i-1]+L[i-2])>200)
    print(fn,"peak",peak,"jumps",jumps,"zeros",sum(1 for x in L if x==0))
P
