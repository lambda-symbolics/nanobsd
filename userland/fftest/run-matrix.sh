#!/bin/sh
# run-matrix.sh x|wayland: per clip, visible/hidden with the panel on, then panel off
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin
T=/var/tmp/fftest; s=$1; tag=$( [ $s = x ] && echo X || echo MH )
grp() { if [ $s = x ]; then k=$( [ $1 = 6 ] && echo super+t || echo super+h ); su mag -c "sh $T/xkey.sh $k"; else su mag -c "sh $T/mhgrp.sh $1" > /dev/null; fi; }
panel() { if [ $s = x ]; then su mag -c "sh $T/xset.sh $1"; else su mag -c "sh $T/mhform.sh $T/mh-$1.lisp" > /dev/null; fi; }
echo "=== matrix $tag $(date)" >> /var/tmp/pmeas.txt
panel on; sleep 5
/var/tmp/pmeas.sh $tag-idle-on 60
for clip in test vp9-1440p60; do
	c=$( [ $clip = test ] && echo 1080p30 || echo 1440p60 )
	page=file:///var/tmp/fftest/play.html; [ $clip = test ] || page=file:///var/tmp/fftest/play-$clip.html
	grp 6; sleep 2
	su mag -c "sh $T/launch.sh $s $page"; sleep 25
	echo "  $c gl: $(sh $T/gl.sh | tr '\n' ' ')" >> /var/tmp/pmeas.txt
	/var/tmp/pmeas.sh $tag-$c-visible-on 60
	grp 5; sleep 10
	/var/tmp/pmeas.sh $tag-$c-hidden-on 60
	grp 6; sleep 5
	panel off; sleep 10
	/var/tmp/pmeas.sh $tag-$c-visible-off 60
	panel on; sleep 3
	sh $T/ffkill.sh; sleep 5
done
panel restore
echo "=== matrix $tag done" >> /var/tmp/pmeas.txt
