#!/bin/sh
export DISPLAY=:0 XAUTHORITY=/home/mag/.Xauthority
x=/usr/X11R7/bin/xset
case $1 in
on) $x s off; $x +dpms; $x dpms force on; $x -dpms ;;
off) $x +dpms; $x dpms force off ;;
restore) $x s default; $x +dpms; $x dpms 300 300 300; $x dpms force on ;;
esac
