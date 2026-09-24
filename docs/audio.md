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
