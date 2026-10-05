#!/bin/sh
# which GL driver each test-firefox process maps
for p in $(pgrep -u mag -f 'fftest/prof') $(pgrep -u mag firefox); do
	l=$(pmap $p 2>/dev/null | grep -o -e '[a-z0-9_]*_dri\.so' -e 'libEGL[^ ]*' -e 'libGLX[^ ]*' | sort -u | tr '\n' ' ')
	[ -n "$l" ] && echo "  $p: $l"
done | sort -u
