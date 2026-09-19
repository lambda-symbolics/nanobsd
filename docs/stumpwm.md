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
Moving a column between workspaces preserves its width and each window's height
share. Focus-loss handling defers relayout until source membership is updated,
so hiding a departing window cannot immediately remap it.

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

## Follow-up verification and deployment

The rc file and both Lisp modules compile successfully on Linux SBCL 2.6.6
against StumpWM 24.11 (`20d839f2ddfdfd25a8460152bc5dc45a9354e773`) and
CLX 0.7.6 (`a444b1278dbd74ea4e6c4846d3ee653e05cccb94`). This was compilation
only, without loading the configuration or starting X. Source review also
covered the request pipe, event dispatcher, geometry updates and workspace
transfer ordering.

The laptop is powered off while travelling. NetBSD compilation, deployment and
live X validation are pending; SSH attempts are paused. No power measurements or
runtime test workloads were run during this follow-up.

When the laptop is available and live validation is authorized:

1. Back up the installed rc, modules and sampler; install the files listed above.
2. Compile on the laptop, reload the rc, and restart the sampler for its restored
   three-second cadence.
3. Check workspace switching and column transfers, including stacked windows with
   unequal heights, offscreen focus, resizing and fullscreen. Check status updates
   and rc reloads for errors in the WM log.

For rollback, restore the backed-up files and restart StumpWM and the sampler.
Removing a module load from the rc is insufficient to undo dispatcher methods
already redefined in a running Lisp image.
