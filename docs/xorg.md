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
