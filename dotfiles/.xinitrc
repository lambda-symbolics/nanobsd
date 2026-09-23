#!/bin/sh
# X session for lispbsd: modesetting/glamor and StumpWM.
export PATH=/usr/local/bin:/usr/pkg/bin:/usr/X11R7/bin:$PATH
# GPU: Mesa iris (Tiger Lake) lives in the pkgsrc Mesa, base libGL/EGL load it from here
export LIBGL_DRIVERS_PATH=/usr/pkg/lib/dri
setxkbmap -layout cz,us -option grp:alt_shift_toggle 2>/dev/null
# The ThinkPad volume keys reach X as keycodes without keysyms (mute 141, down 142,
# up 8; xkb also puts 8 in mod3), so StumpWM's XF86Audio* bindings never matched.
# StumpWM grabs one keycode per keysym, so take the keysyms off the xkb defaults
# (160/174/176, never sent by this keyboard) and put them on the real ones.
xmodmap -e 'keycode 160 = NoSymbol' -e 'keycode 174 = NoSymbol' -e 'keycode 176 = NoSymbol' \
        -e 'keycode 141 = XF86AudioMute' -e 'keycode 142 = XF86AudioLowerVolume' \
        -e 'clear mod3' -e 'keycode 8 = XF86AudioRaiseVolume' 2>/dev/null
xset r rate 300 40
xset s off
[ -r "$HOME/.Xresources" ] && xrdb -merge "$HOME/.Xresources"
hsetroot -solid '#0A0A0A'
# Give the root window a cursor.  StumpWM never defines one, so until some
# client maps a window with its own cursor, the pointer over bare root is
# whatever X inherited - which on this machine is nothing visible at all.
xsetroot -cursor_name left_ptr
# static warm display colour (~4000K), reapplied across DPMS blanks
pgrep -f statusbard >/dev/null || /usr/local/bin/statusbard &
/usr/local/bin/screentemp 4000 -d &

/usr/local/bin/brightness restore >/dev/null 2>&1
# the mixer resets to the codec default on every boot; replay the last setting
/usr/local/bin/volume restore >/dev/null 2>&1
xset +dpms 2>/dev/null
xset dpms 300 300 300 2>/dev/null
exec stumpwm >> "$HOME/.xsession-errors" 2>&1
