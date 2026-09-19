# Xorg presentation and display power

Configured on 2026-09-19 for the Intel Xe laptop running NetBSD.

## Configuration

Install `picom` with `pkgin install picom`. Deploy `dotfiles/picom.conf` to
`/home/mag/.config/picom.conf` and `dotfiles/.xinitrc` to `/home/mag/.xinitrc`.
The session script starts one compositor as the desktop user and enables DPMS
with five-minute standby, suspend and off timeouts.

Xorg uses modesetting with hardware glamor acceleration and page flipping.
The installed driver has no native TearFree option. Picom uses XRender with
VSync through synchronized Present, damaged-region repainting, and fullscreen
compositing. Shadows, fading, blur, rounded corners and opacity changes are
disabled. Native 2160×1350 at 59.74 Hz and the existing colour-temperature
configuration are retained.

Both GLX and EGL diagnostics lacked buffer-age support on this installation.
XRender was selected to use the existing Xorg acceleration without relying on
that extension. Picom's XRender startup message about disabling frame pacing
refers to its separate timing scheduler, not Present synchronization. See the
[Picom v11 XRender implementation](https://raw.githubusercontent.com/yshui/picom/v11/src/backend/xrender/xrender.c).

Compositing adds graphics work. The existing display sleep settings were already
appropriate; no battery savings were measured or claimed for this change.
Subsequent GPU/power comparisons must account for the running compositor.

## Verification and operation

Local and NetBSD shell syntax checks passed. Hardware Intel Xe rendering was
confirmed with `glxinfo -B` and Picom diagnostics. The running compositor owned
`_NET_WM_CM_S0`; an isolated Alacritty client rendered correctly and closed without
stopping Picom. Native mode and DPMS settings were checked after deployment.
This verifies configuration and rendering, not physical scanout tearing.

Read the compositor log at `/home/mag/.stumpwm.d/picom.log`. One BadWindow warning
was observed during test-client destruction; the compositor continued running.

To revert, restore `/home/mag/.xinitrc.before-picom-20260919` to
`/home/mag/.xinitrc`, then stop the desktop user's compositor with
`pkill -u mag -f '(^|/)picom([[:space:]]|$)'`. No Xorg restart is needed to stop it.
