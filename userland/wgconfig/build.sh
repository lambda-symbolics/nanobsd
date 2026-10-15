#!/bin/sh
# Build wgconfig with the persistent-keepalive option against the base system headers; install to /usr/local/sbin.
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -o wgconfig wgconfig.c -lprop -lutil
install -m 555 wgconfig /usr/local/sbin/wgconfig
echo "installed /usr/local/sbin/wgconfig"
