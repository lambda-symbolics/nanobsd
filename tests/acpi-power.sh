#!/bin/sh
# Compile the production ACPI power backend against deterministic host AML.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/acpi-power.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -Wpedantic -pthread ${CFLAGS:-} \
    "$root/tests/acpi-power-harness.c" -o "$tmp/acpi-power"
"$tmp/acpi-power"
