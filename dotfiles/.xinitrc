#!/bin/sh
# X session for lispbsd: modesetting/glamor and StumpWM.
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/X11R7/bin:$PATH
# GPU: Mesa iris (Tiger Lake) lives in the pkgsrc Mesa, base libGL/EGL load it from here
export LIBGL_DRIVERS_PATH=/usr/pkg/lib/dri
setxkbmap -layout cz,us -option grp:alt_shift_toggle 2>/dev/null
xset r rate 300 40
xset s off
[ -r "$HOME/.Xresources" ] && xrdb -merge "$HOME/.Xresources"
hsetroot -solid '#0A0A0A'
# static warm display colour (~4000K), reapplied across DPMS blanks
pgrep -f statusbard >/dev/null || /usr/local/bin/statusbard &
/usr/local/bin/screentemp 4000 -d &

/usr/local/bin/brightness "$(cat "$HOME/.brightness" 2>/dev/null || echo 20)" >/dev/null 2>&1
xset +dpms 2>/dev/null
xset dpms 300 300 300 2>/dev/null
exec stumpwm >> "$HOME/.xsession-errors" 2>&1
