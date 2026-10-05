#!/bin/sh
# stop the test firefox via its profile lock (firefox rewrites argv)
p=$(readlink /var/tmp/fftest/prof/lock 2>/dev/null | sed 's/.*+//')
[ -n "$p" ] || exit 0
kill -TERM $p 2>/dev/null
i=0; while kill -0 $p 2>/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done
kill -KILL $p 2>/dev/null; true
