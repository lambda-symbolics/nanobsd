#!/bin/sh
set -eu
root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/state" "$tmp/run"
cat > "$tmp/bin/sysctl" <<'STUB'
#!/bin/sh
[ "${BRIGHTNESS_FAIL:-0}" = 0 ] || exit 1
printf '%s\n' "${2#*=}" >> "$TRACE"
STUB
chmod +x "$tmp/bin/sysctl"
# Replace only the OS boundary in a private copy. Never invoke doas or sysctl.
sed -e "s|^AC_FILE=.*|AC_FILE=$tmp/ac|" \
    -e 's/^SUDO=.*$/SUDO=/' -e "s|/sbin/sysctl|$tmp/bin/sysctl|g" \
    "$root/userland/brightness" > "$tmp/brightness"
export BRIGHTNESS_STATE_DIR=$tmp/state BRIGHTNESS_RUN_DIR=$tmp/run TRACE=$tmp/trace
run() { sh "$tmp/brightness" "$@"; }
for ac in 1 15; do
	printf '%s\n' "$ac" > "$tmp/state/.brightness.ac"
	echo 71 > "$tmp/state/.brightness.bat"
	echo 1 > "$tmp/ac"; : > "$TRACE"
	run dim 15
	[ ! -s "$TRACE" ] && [ "$(cat "$tmp/run/brightness.dimmed")" = 15 ] || exit 1
	echo 0 > "$tmp/ac"; [ "$(run restore bat)" = 15 ]
	[ "$(run restore ac)" = "$ac" ]
	if BRIGHTNESS_FAIL=1 run undim 2>/dev/null; then exit 1; fi
	[ -f "$tmp/run/brightness.dimmed" ]
	[ "$(run undim)" = 71 ]
	[ ! -e "$tmp/run/brightness.dimmed" ]
	[ "$(cat "$TRACE")" = "$(printf '%s\n' 15 "$ac" 71)" ]
done
if BRIGHTNESS_FAIL=1 run dim 15 2>/dev/null; then exit 1; fi
[ ! -e "$tmp/run/brightness.dimmed" ]
if BRIGHTNESS_FAIL=1 run +10 2>/dev/null; then exit 1; fi
[ "$(cat "$tmp/state/.brightness.bat")" = 71 ]
run dim 15; [ "$(run restore ac)" = 15 ]; [ "$(run restore bat)" = 15 ]
echo ok
