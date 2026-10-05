#!/bin/sh
# Build upstream wpa_supplicant 2.11 with the RSNXE check off and install it
# as /usr/local/sbin/wpa_supplicant-2.11 (see docs/braiins-wifi.org).
# Run as root from this directory.  /etc/rc.conf.d/wpa_supplicant makes the
# stock rc.d script start it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-/var/tmp/wpa211}
mkdir -p "$work" && cd "$work"
[ -f wpa_supplicant-2.11.tar.gz ] ||
    ftp -o wpa_supplicant-2.11.tar.gz https://w1.fi/releases/wpa_supplicant-2.11.tar.gz
want=912ea06f74e30a8e36fbb68064d6cdff218d8d591db0fc5d75dee6c81ac7fc0a
got=$(cksum -a SHA256 wpa_supplicant-2.11.tar.gz | awk '{print $NF}')
[ "$got" = "$want" ] || { echo "SHA256 mismatch: $got" >&2; exit 1; }
rm -rf wpa_supplicant-2.11
tar xzf wpa_supplicant-2.11.tar.gz
cd wpa_supplicant-2.11
patch -p0 < "$here/netbsd-rsnxe.patch"
cp "$here/config" wpa_supplicant/.config
cd wpa_supplicant
gmake -j8 wpa_supplicant
install -m 755 wpa_supplicant /usr/local/sbin/wpa_supplicant-2.11
