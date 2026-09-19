# Scrolling StumpWM on the Nano

The desktop on `root@10.0.0.25` runs as `mag`, with StumpWM 24.11.
Install `dotfiles/.stumpwmrc` as `/home/mag/.stumpwmrc`, and install
`dotfiles/scrolling.lisp` and `dotfiles/x11-efficiency.lisp` under
`/home/mag/.stumpwm.d/`.
Install `userland/statusbard` as `/usr/local/bin/statusbard`.
Reload the rc file with StumpWM's `loadrc` command.

Each of the six workspaces has its own ordered columns and viewport offset.
Open a window to insert a column after the focused column. Columns keep their
width when more windows open; focus a column beyond the panel edge to scroll.
Stack windows vertically using consume/expel. The viewport targets the Nano's
single panel. Focus changes and geometry updates are instantaneous.

## Keys

These WM bindings follow `/root/.config/niri/config.kdl` on the Linux host.
Use Super as Mod.

| Keys | Action |
| --- | --- |
| Super + Left/Right | Focus previous/next column |
| Super + Up/Down | Focus previous/next window in the column |
| Super + Ctrl + arrows | Reorder columns horizontally or windows vertically |
| Super + Home/End | Focus first/last column |
| Super + Ctrl + Home/End | Move column to first/last position |
| Super + C/I/E/A/H/T | Select workspaces 1 through 6 |
| Super + Ctrl + C/I/E/A/H/T | Move the entire column to that workspace |
| Super + comma | Consume the first window of the next column |
| Super + period | Expel the focused window into a new column |
| Super + R | Cycle widths: one third, one half, two thirds |
| Super + D/W | Decrease/increase column width by 5 percentage points |
| Super + Shift + D/W | Decrease/increase window height share by 5 percentage points |
| Super + F | Toggle full-width column |
| Super + Shift + F | Toggle fullscreen window |
| Super + Shift + C | Center the focused column |
| Super + Q | Close window |
| Super + O / Shift + O | Panel on/off |
| Super + Shift + P | Panel off |

Default column width is 30%, with 16-pixel gaps and one-pixel borders.
Use keyboard or click focus; pointer motion does not change the viewport.

## Idle work

Layout runs on window/workspace events and keyboard commands. Fully offscreen
windows are unmapped. During pure scrolling, only parent positions are changed;
client sizes are configured only when resizing. Synthetic ConfigureNotify uses
the known geometry instead of querying X for it. There is no animation timer.

Status sampling and reading run every three seconds. A worker reads the snapshot
and uses StumpWM's existing request pipe to queue a main-thread callback. Pending
updates are coalesced. Formatting uses an in-memory snapshot, and the bar is
rendered only when its formatted contents change. Reloading retires the older
X-connection worker cooperatively.

The earlier refresher sent synthetic Expose events, which force a repaint in
StumpWM even when the contents are identical. The request-pipe implementation
avoids those forced repaints and the second X connection. The native mode-line
timer is cancelled because of the previously observed finite-timeout busy-poll
on this NetBSD/SBCL build.

StumpWM 24.11's display-channel dispatcher calls `display-finish-output` before
each event and before checking an empty queue. That operation waits for the X
server to process requests. `x11-efficiency.lisp` uses `display-force-output`
in those two dispatcher methods, then waits for readiness in the I/O loop.
See the [upstream dispatcher](https://github.com/stumpwm/stumpwm/blob/24.11/stumpwm.lisp)
and [CLX output semantics](https://sharplispers.github.io/clx/Managing-the-Output-Buffer.html).

## Graphics-stack inspection, 2026-09-19

The live Xorg configuration uses `modesetting`, glamor, and page flipping.
The startup log identifies hardware acceleration on Intel Xe/TGL, with Present,
DRI3 and DPMS initialized. It also reports an AIGLX initialization failure and
loading the DRISWRAST GLX provider. That identifies a software server-side GLX
fallback, but does not establish the renderer selected by a direct DRI3/EGL
application. The Xorg driver configuration was inspected rather than changed.

The initial scrolling deployment was compiled and loaded on the laptop; its
backups use the suffix `.before-scrolling-20260919`.

The follow-up request-pipe, asynchronous-dispatch and geometry changes have had
source review and syntax checks. Remote compilation and deployment are pending:
SSH to `10.0.0.25` became unreachable during the follow-up. Runtime test workloads
and power measurements were omitted while kernel power work was in progress.
