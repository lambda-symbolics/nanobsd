#!/bin/sh
# libsndio (pkgsrc audio/sndio 1.10.0nb1) with the LISPBSD fixes for NetBSD
# audio(4): mpv's sndio output hung on every seek, pause and track switch
# (docs/mpv-sndio.org).  The pkgsrc package uses libsndio's OSS backend over
# ossaudio(3), and two of its assumptions do not hold there:
#  - sio_flush() only paused the device (SNDCTL_DSP_SETTRIGGER) and left the
#    queued samples; the next blocking sio_write() waited for ever on a
#    paused device that libsndio never started.  Discard with
#    SNDCTL_DSP_HALT_OUTPUT (AUDIO_FLUSH) first.
#  - the device is started when the buffer is exactly full; audio(4) moves up
#    to four blocks out of the user ring as soon as they are written and
#    reports them as gone, so mpv (writes in whole blocks of the reported
#    space) could stop a block short and nothing started.  Start when within
#    one block of full.
# patch-lispbsd-libsndio_sio__oss.c goes next to pkgsrc's own patches; the
# distinfo checksum must be regenerated.  `make replace` swaps the installed
# package in place and leaves mpv (which links libsndio.so.7) alone.
# Never run pkgin or pkg_add while this runs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd /usr/pkgsrc/audio/sndio
cp "$here/patch-lispbsd-libsndio_sio__oss.c" patches/
make makepatchsum
make clean
make replace
make clean
