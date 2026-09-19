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
Focus follows the cursor (`:sloppy`); entering a window focuses it and reveals
its column as needed. Use uppercase keysyms for shifted letters in the rc, for
example `s-F` for Super+Shift+F.

## Idle work

Layout runs on window/workspace events and keyboard commands. Fully offscreen
windows are unmapped. During pure scrolling, only parent positions are changed;
client sizes are configured only when resizing. Synthetic ConfigureNotify uses
the known geometry instead of querying X for it. There is no animation timer.
After all moves and unmaps, send one Expose notification to each visible client
whose geometry or visibility changed. This prevents stale contents after column
rearrangement; an unchanged layout sends none.

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

Follow-ups `123fb13` and `29b36c5` were deployed over WireGuard on 2026-09-19.
All three Lisp files compiled in the laptop's SBCL 2.6.5 StumpWM image with no
warnings or failures. The rc reload succeeded, and installed file checksums
matched the repository sources. The sampler was restarted as `mag`.

Live inspection confirmed six `strip-group` workspaces, the `:pipe` status
backend at a three-second interval, one refresher thread, no pending status
callback and no native mode-line timer. The snapshot timestamp advanced by three
seconds, and neither WM log grew during deployment. No power measurements or
application test workloads were run.

The initial deployment had six empty workspaces. Subsequent user checks found
incorrect shifted-letter bindings and missing pointer focus. Both were corrected,
compiled without warnings on NetBSD, loaded, and confirmed in the live keymap and
focus-policy setting after a reboot. The preceding rc and scrolling module are
backed up under `/home/mag/.stumpwm.d/before-shift-focus-fix/`.

The stacking redraw issue was reproduced with five Alacritty windows, the pointer
on the bar, and Super+Home, Super+comma, then Super+period. At the right edge, an
unmapped window's old pixels covered a moved neighbour. X geometry and map state
were correct, but the neighbour's log showed a move without a redraw. A targeted
Expose cleared the stale pixels without changing focus.

The scrolling module now requests client redraws after completing layout changes.
The updated module compiled without warnings on NetBSD. Keyboard stacking,
unstacking and width changes repainted correctly in screenshots, and event logs
confirmed immediate redraws. An instrumented unchanged layout emitted zero
redraw notifications. The source and FASL were installed, checksums verified, and
all diagnostic clients were closed. The preceding source is backed up as
`/home/mag/.stumpwm.d/scrolling.lisp.before-redraw`.

The pre-follow-up files are backed up under
`/home/mag/.stumpwm.d/before-followup-29b36c5/`. Staged sources, FASLs and
`compile-report.sexp` are under `/home/mag/.stumpwm.d/followup-29b36c5/`.

For rollback, restore the backed-up files and restart StumpWM and the sampler.
Removing a module load from the rc is insufficient to undo dispatcher methods
already redefined in a running Lisp image.

## Input-lag and efficiency audit, 2026-09-19

The custom commands contain no sleeps or synchronous shell-command waits.
Status collection runs in a separate process; snapshot reads run in the worker,
not the X event thread. Ordinary application typing is not routed through the
WM command handler. The custom code does not generate key presses or releases.
Layout changes can still wait on Xorg through inherited window/focus operations;
a stalled X server, input stack, application or scheduler requires separate
runtime evidence. The audit did not reproduce or establish the cause of the
reported multi-second input lag.

The live WM had one status worker, no queued status callback and no active timer.
An isolated SBCL 2.6.5 finite-select check waited 109 ms for a requested 100 ms.
This did not reproduce the older busy-poll observation; the existing timer
workaround was not changed.

`userland/statusbar` now collects each subsystem once and parses all results in
one awk process: seven external utility invocations per sample, including the
parser. In particular, three envstat invocations become one, and individual
mixer queries become one mixer snapshot. Fields and three-second cadence are
preserved. Install it as `/usr/local/bin/statusbar`; the running sampler uses the
new script on its next iteration. No battery-power improvement was measured.

The column layout now traverses each window list once to identify its final
member, rather than repeatedly scanning to the end. Geometry and redraw rules
are unchanged. Last-member selection matched the old algorithm for list lengths
0 through 100. The module compiled without warnings or failures on NetBSD and
was loaded into the running WM; deployed source checksums matched.

Run parser fixtures with `sbcl --script tests/statusbar.lisp`. They passed with
Linux and NetBSD awk, covering CPU deltas/reset, RAM cache exclusion, hottest
core, mixer fallbacks, battery states and missing samples. Live status publication
also passed. Backups and staged files are in
`/home/mag/.stumpwm.d/efficiency-audit/` (`statusbar.before` and
`scrolling.lisp.before`).
