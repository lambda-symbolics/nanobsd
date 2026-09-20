# nanobsd

LISPBSD: a working **NetBSD 11.0/amd64** setup on the **Lenovo ThinkPad X1 Nano
Gen 1** (Tiger Lake i7-1160G7, Iris Xe, AX201 WiFi, Elan I2C touchpad), with
root running `cclsh` (SBCL) as its login shell and StumpWM as the WM.

This repo archives the out-of-tree kernel patches, loadable modules, userland
helpers and dotfiles that make the machine usable — for posterity, not as a
buildable tree. Kernel files live under `kernel/` at their `src/sys/`-relative
paths.

## Layout
- `kernel/modules/hwp` — Intel Speed Shift (HWP) governor; gives `machdep.hwp.epp`
  and turbo (Tiger Lake has no legacy SpeedStep, so `estd` is dead here).
- `kernel/modules/patfix` — broadcasts IA32_PAT to all CPUs (NetBSD 11 only
  programs PAT on the BSP → write-combining framebuffer was uncached on APs).
- `kernel/modules/cidle` — MWAIT deep C-state idle loop (acpicpu only picks C1);
  cores now reach C7, and exposes `machdep.cidle.residency` (core/package C-states).
- `kernel/kern/kern_lpsched.c`, `kernel/sys/lpsched.h` + hooks in `kern_runq.c`,
  `kern_timeout.c`, `wskbd.c`/`wsmouse.c` — **lpsched**, a laptop power governor:
  idle-aware thread packing, callout coalescing and an input-idle signal under
  `machdep.lpsched.*`, driven by `userland/lpschedd.c` (AC/load/idle → HWP EPP +
  profile) with a StumpWM focus hook (`dotfiles/.stumpwm.d/lpsched.lisp`).
  Design: `docs/lpsched-design.org`.
- `kernel/external/.../i915` — Tiger Lake i915 bring-up + RC6 power-gating patch
  (package now reaches PC8).
- `kernel/dev/acpi` — forced-S3 SSDT injection (`options ACPI_FORCE_S3`) and
  C-state tweaks. NOTE: S3 enters but does not resume on this firmware.
- `kernel/dev/pci/if_iwx.c` — WiFi power management (adds IEEE80211_C_PMGT).
- `kernel/dev/pckbport`, `kernel/arch/x86/pci/dwiic_pci.c` — Elan touchpad.
- `userland/` — `brightness` (ACPI backlight, save/restore), `iblc`, status bar
  (`statusbar`/`statusbard`), `screentemp` (static 4000K), `s3diag`.
- `dotfiles/` — `.stumpwmrc` (waybar-style bar, no-busy-loop mode-line refresh),
  `.xinitrc`.
- `etc/boot.cfg`, `etc/rc.conf`, `etc/sysctl.conf`, `etc/modules.conf`,
  `etc/rc.d/`, `etc/powerd/` — boot menu, enabled services (`lpschedd`),
  persisted sysctls (HWP EPP, tickless idle) and autoloaded modules.
- `docs/` — running notes, including `lpsched-design.org` (design, review,
  repairs and measurements) and `rpm-s0ix-findings.org`.

Power tuning highlights: fixed a StumpWM mode-line busy-loop (~2.5 W), enabled
deep C-states + RC6, HWP EPP bias, and ACPI backlight control → idle ~5 W
screen-on (WiFi), from ~10 W.
