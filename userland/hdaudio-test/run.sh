#!/bin/sh
# run.sh [path/to/hdaudio.c]: extract the functions under test and run them.
set -eu
cd "$(dirname "$0")"
src=${1:-../../kernel/dev/hdaudio/hdaudio.c}
./extract.sh "$src" > extracted.inc
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter ${CFLAGS:-} \
    -D_DEFAULT_SOURCE hdaudio-test.c -o hdaudio-test
./hdaudio-test "$@"
