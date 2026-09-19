# Scrolling StumpWM on the Nano

The desktop on `root@10.0.0.25` runs as `mag`, with StumpWM 24.11.
Install `dotfiles/.stumpwmrc` as `/home/mag/.stumpwmrc` and
`dotfiles/scrolling.lisp` as `/home/mag/.stumpwm.d/scrolling.lisp`.
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
windows are unmapped; visible clients receive geometry changes only when needed.
There is no animation or layout timer.

Status sampling is once per 60 seconds, down from 3 seconds. Bar refresh is once
per 60 seconds, down from 15 seconds, through a separate X connection. The WM's
mode-line timer is cancelled to avoid the existing NetBSD/SBCL finite-timeout
busy-poll problem. Reloading from the older configuration retires its refresher
cooperatively, without interrupting an SBCL thread. Per-redraw debug writes to
`/tmp/ml-render` have been removed.

Deployment backups use the suffix `.before-scrolling-20260919`.
Compilation, syntax checks and live configuration inspection were used for this
change; runtime test workloads and power measurements were omitted while kernel
power work was in progress.
