# Xorg presentation and display power

Configured on 2026-09-19 for the Intel Xe laptop running NetBSD.

Deploy `dotfiles/.xinitrc` to `/home/mag/.xinitrc`. The session uses StumpWM
without a compositor and enables DPMS with five-minute standby, suspend and off
timeouts.

Xorg uses modesetting with hardware glamor acceleration and page flipping.
The installed driver has no native TearFree option. The display runs at native
2160×1350 at 59.74 Hz with the existing colour-temperature configuration.
Hardware Intel Xe rendering was confirmed with `glxinfo -B`.

Picom was briefly tested with XRender and synchronized Present, then removed at
the user's request. Its package, configuration and session autostart were removed.
GLX and EGL diagnostics during that trial lacked buffer-age support. Power
measurements taken while Picom was running include its compositing overhead;
subsequent measurements use the uncomposited desktop.

## Blanking and Firefox video

The X screensaver is off (`xset s off`) and DPMS blanks after five minutes.
Firefox asks the display to stay on while a video plays. On this desktop it
has no `org.freedesktop.ScreenSaver`, GNOME or portal service to ask, so it
falls back to the X `MIT-SCREEN-SAVER` extension and `XScreenSaverSuspend`,
which stops both the screensaver and DPMS timers in the server itself.

That fallback needs `libXss.so.1`, loaded by name at run time. NetBSD ships
the same library as `/usr/X11R7/lib/libXss.so.2`, so the lookup failed and
Firefox silently gave up. The compatibility link

    ln -s /usr/X11R7/lib/libXss.so.2 /usr/pkg/lib/firefox/libXss.so.1

sits in the first entry of Firefox's run path and repairs it. Firefox picks
the mechanism once per process, so it must be restarted after adding the
link. With `MOZ_LOG=LinuxWakeLock:5` the log shows the switch to
`XScreenSaver`. Reapply the link after a Firefox package upgrade if it
removes the directory.
