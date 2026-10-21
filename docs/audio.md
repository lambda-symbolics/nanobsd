# Audio on the Nano: buzz after suspend

Codec: Realtek ALC287 (`hdafg0`, codec 0) plus the Intel HDMI codec in the GPU
(`hdafg1`, codec 2) on the PCH HD Audio controller (`hdaudio0`, 00:31:3).
Playback runs at 48 kHz, 16 bit, 10 ms blocks; the hardware ring is three
blocks (30 ms).

## Symptom

After an s2idle suspend and resume every output carried a buzz. Speech sounded
chopped and slightly slowed, and a pure 440 Hz test tone was heard correctly
with the buzz on top. A cold boot cleared it. Before the cause was found this
looked random, because it only followed a lid close.

## What was ruled out

The DMA data was captured straight out of the ring while the buzz played
(`ringcap` polls the link position and copies each completed block): six
seconds with no silent block, no repeated block, no discontinuity and a smooth
level. Interrupts ran at a steady 100/s, the mixer rewrote every block on
time, Firefox's audio threads waited normally on the device, and the mixer
controls never changed. The fault therefore sat in the codec or amplifier,
not in the kernel mixer, the scheduler or Firefox.

Two things looked like audio stalls and were not. The `intrctl` counter for
the controller's MSI vector stops for seconds at a time while interrupts are
demonstrably being served; treat it as unreliable. The i915 vblank interrupt
pauses whenever nothing repaints.

## What is known so far

`hdaudio_resume()` used to pull the link reset (`CRST`) on every resume,
which resets both codecs and made the driver talk to the HDMI codec while
the GPU was still dark (twelve 150 ms command timeouts per resume). Kernel
123 skips the reset when the controller kept its state, which it always
does here: the audio controller is not in the s2idle power-down list and its
PCI power capability sets No_Soft_Reset. That change is correct and stays,
but it did not remove the buzz.

Kernel 124 adds a raw-verb ioctl (`HDAUDIO_FGRP_COMMAND`) and
`userland/hdaverb`. With it, every widget register and all 128 Realtek
vendor coefficients were dumped before and after timed freezes, compared
like-for-like (idle against idle, playing against playing): nothing changes.
Coefficient `0x30` differs between idle and playing, `0x77`/`0x78` are
volatile readbacks; neither is a resume effect. The PCH clock-gating bit the
suspend hook sets (`CGCTL` bit 6) comes back in either state after a resume,
but the controller's 24 MHz wall clock runs at exactly 24 MHz with the bit
set or clear during playback, so gating does not stall the link.

The internal microphones are not on the HDA codec, so the buzz cannot be
detected remotely. The decisive observation came from the user listening
during a ten-minute automated freeze: the tone buzzed after the wake and
became clean again while the post-resume codec dump ran. The dump only
reads, except for the `SET_COEFFICIENT_INDEX` writes that walk the Realtek
vendor coefficient interface, so that walk is the cure; the firmware gives
the codec a similar nudge at boot.

## Fix

`kernel/dev/hdaudio/hdafg.c`: `hdafg_realtek_kick()` walks
`SET_COEFFICIENT_INDEX` / `GET_PROCESSING_COEFFICIENT` over all 128 indices on
the vendor node (0x20) at the end of `hdafg_resume()` for Realtek codecs and
logs `resume: vendor coefficient interface walked`. Kernel 125
(`/netbsd.i915.rpm.125`) carries it together with the kernel 123 and 124
changes and is the default boot entry. Not yet confirmed by ear after a
reboot; if the buzz survives a suspend on kernel 125, the bisect script
`/var/tmp/bisect.sh` applies the candidate operations one per 15 s after a
timed freeze so the exact step can be identified by listening.

## Do not detach a live hdafg

Trying to recover with `drvctl -d hdafg0` after a bad resume hung the detach
in `kthread_join` on the headphone-sense polling thread, left `drvctl` in a
D state, and later blocked the reboot after "syncing disks... done" until a
power cycle. Reboot instead.

## Probes left in /var/tmp on the laptop

`hdaregs [stream]` dumps controller and stream registers (mmap of `/dev/mem`;
plain reads of MMIO are refused). `hdabuf [x]` prints the BDL or hashes of
the three ring blocks. `ringcap SECONDS` captures the played PCM to
`ring.raw`. `hdactl` dumps CORB/RIRB state. `tone.raw` is 3 s of 440 Hz for
`audioplay -f -e slinear_le -P 16 -s 48000 -c 2`. `fakekey KEYCODE` injects a
key via XTEST. All register access is read-only.

## 2026-10-08: the "crunch" was the mixer level mapping (kernel 140)

Symptom since September: hiss/crunch that scales with loudness, starts
"randomly" during use, then every player crunches (Firefox on 10-07, mpv
with a local E-AC-3 episode on 10-08), persists across player restarts,
never from a fresh boot.  The 10-07 work had shown the digital path to be
faithful (DMA ring == client samples, no gaps, no clipping) and the codec
registers identical to a clean dump.

Today's capture of the DMA ring during the crunch (`/var/tmp/ringcap`,
analysed on the workstation) again showed clean program audio: no
full-scale samples, no zero runs, no discontinuities, same spectrum and
crest factor as the ffmpeg decode of the track.  The only register
difference from the clean September dump was the two DAC output
amplifiers (nid 0x02/0x03): index 0x5f, where the amplifier capability
(0x25757) says 88 steps, so the last valid index is 0x57 (0 dB).

`hdafg_set_port` computed `divisor = 255 / ctl_step` in integer
arithmetic: 255/87 = 2, so the amplifier index was `level / 2` and every
master level above 174 ran past the last step; the Realtek DAC treats
that as gain above 0 dB and clips loud passages.  The mixer read back
`index * 2`, so `mixerctl` showed a plausible 190 all along.  "Random
start under load" was the moment the volume keys took the master from
under 174 to over it; it persisted because the level stayed.

Kernel 140 maps 0..255 onto 0..ctl_step exactly (rounded, clamped) and
inverts it in `hdafg_get_port`.  Until the reboot, keep `outputs.master`
and `outputs.master2` at 174 or below.  Also found on the way:
audio(4) remembers the last pause flag for `/dev/sound`, so a test
`audioplay` started while mpv is paused begins paused and hangs in
`audiowr`; use `-d /dev/audio`.  And a Firefox content process writes
digital silence at 35 % of a core while a stale media tab exists.

## 2026-10-08 evening: codec power-down is not a lever (AC)

hdafg never leaves D0 at runtime (D0 on attach and resume only); the ALC287
reports D0-D3 and D3cold supported.  Setting the AFG to D3 by raw verb
(`hdaverb 0 1 0x705 3`, widgets follow, readback 0x233) with nothing holding
/dev/audio and alternating five 30 s D0/D3 windows on AC gave package 2.0-3.4 W
in both states; the paired differences were -0.31, -0.55, +0.54, -0.51 and
+0.66 W, i.e. noise.  pc10 was reached with the codec in D0, so neither the
codec nor the hdaudio controller in D0 blocks package C-states here.  The
codec's own draw is off-package and does not show in RAPL; if it matters at
all it is a battery-discharge A/B, tens of mW at most.  Firefox's silent
35 %-CPU content process (stale media tab, seen 10-08 afternoon) died with
the reboot and was not reproducible on demand; Firefox has the sndio and
sun cubeb backends built in, backend pref unset.

## 2026-10-21: the audio ring could fall a block behind the DMA (kernel 189)

Astra's review (2026-10-19, kept in /root/audio-bug-review on the workstation,
reproduced against the old code with mocked hardware) found two defects that
together fit "playback goes bad and stays bad until the stream restarts":

1. `hdafg_stream_intr` cleared SD_STS without looking at it and called the
   audio layer's completion callback exactly once per interrupt.  audio(4)
   advances its hardware ring one block per callback and refills two blocks
   ahead, so an interrupt serviced more than one 10 ms block late (two
   completions merged into one) left the ring one block behind the DMA
   engine for good, refilling the block being played; and a FIFO or
   descriptor error interrupt counted as a completion.
2. A codec command spun up to 50 ms for its reply holding `sc_corb_mtx` at
   IPL_AUDIO (five audio blocks with the stream interrupt held off on that
   CPU), a timed-out command's late reply was taken by the next command,
   unsolicited responses were dispatched inside the reply wait (a nested
   command could take the outer reply) with their tag truncated to the low
   byte, and a failed pin-sense read (0xffffffff) meant "headphones in,
   speakers off".

Fixes (kernel/dev/hdaudio, host test userland/hdaudio-test):
- Completions are counted from the DMA fetch position, not interrupts:
  (LPIB + FIFOS) / blksize against the last accounted block, plus whole laps
  from the time since the last interrupt; the callback runs once per
  completed block, never for an error-only interrupt.  Measured with
  userland/hdaudio-test/bcis-probe.c: at every BCIS, LPIB is 172-192 bytes
  short of the boundary and FIFOS is 192, i.e. the interrupt follows the
  fetch and LPIB trails it by the FIFO.  Using LPIB alone counted every
  interrupt as "early" and, in the test's late-interrupt simulation, left the
  ring off the DMA block 903 times in 10000 periods (kernel 188 had that; 189
  has the FIFOS correction: 0).
- Commands: stale replies are drained before sending, replies from another
  codec are dropped, after a timeout the next command waits 1 ms and drops
  the late reply, and the reply wait is 5 ms.  Unsolicited responses are
  queued (the interrupt handler only does so if the command lock is free)
  and run from a soft interrupt with the full response (tag = bits 31:26).
  A failed pin sense keeps the last routing.
- The /dev/hdaudioN ioctls and `set_format` hold a runtime PM reference.
- `sysctl hw.hdaudio.stats`: cmd_timeouts, stale/foreign replies, unsolicited
  events/drops, stream_irqs, stream_late (blocks beyond one per interrupt),
  stream_early, stream_laps, FIFO and descriptor errors, spurious.  Late
  interrupts and stream errors are also logged to dmesg (at most every 10 s).

When the sound goes bad again: note the time, then `sysctl hw.hdaudio.stats`
and `dmesg | grep hdaudio`.  stream_late/laps growing means the interrupt was
held off (now recovered automatically); cmd_timeouts means the codec stopped
answering; neither moving means the fault is elsewhere (codec analog state,
the application).

Still open (not changed here): audio(4) calls the driver's open with its
interrupt spin lock held, and `hdafg_open` takes the runtime PM adaptive mutex
and may resume the controller there (latent since kernel 153; uncontended in
practice).  A descriptor error stops the DMA and is only logged, not restarted.
