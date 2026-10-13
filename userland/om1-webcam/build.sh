#!/bin/sh
# Builds om1-webcam against pkgsrc libusb-1.0 and installs it.
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -o om1-webcam om1-webcam.c -I/usr/pkg/include -L/usr/pkg/lib -Wl,-R/usr/pkg/lib -lusb-1.0 -lpthread
install -m 755 om1-webcam /usr/local/bin/om1-webcam
install -m 755 om1 /usr/local/bin/om1
install -m 755 devpubd-hook-om1 /libexec/devpubd-hooks/03-om1
echo installed
