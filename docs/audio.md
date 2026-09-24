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

## Cause

`hdaudio_resume()` always pulled the link reset (`CRST`). That resets every
codec, and `hdafg(4)` re-applies only what it knows: power state, pin
configuration, amplifier and connection settings, stream formats. The
vendor-specific initialisation the firmware performs at boot is gone after
the reset, and the ALC287 buzzes without it. The reset also made the driver
talk to the HDMI codec while the GPU was still dark, which cost twelve
150 ms command timeouts per resume (`no response codec=2`).

The controller itself keeps its state across the sleep: the s2idle power-down
list does not include the audio controller, and its PCI power capability sets
No_Soft_Reset, so `GCTL` still has `CRST` set when the resume handler runs.

## Fix

`kernel/dev/hdaudio/hdaudio.c`: on resume, read `GCAP` and `GCTL` first. If the
controller answers and `CRST` is still set, stop the command rings and skip
the reset; the rest of the resume (ring configuration, interrupt enable,
`hdafg_resume`) runs unchanged. Only a controller that really lost its state
is reset. Kernel #123 (`/netbsd.i915.rpm.123`) carries it, and it logs either
`resume: controller state kept` or `resume: controller lost its state`.

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
