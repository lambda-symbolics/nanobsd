#!/bin/sh
# lpsched accounting test: 4 busy loops + a focused periodic waker.
cd "$(dirname "$0")"
cc -O2 -Wall -o waker waker.c || exit 1
sysctl -w machdep.lpsched.enabled=1 >/dev/null
for pack in 0 2 0 2; do
	sysctl -w machdep.lpsched.pack=$pack machdep.lpsched.latency_reset=1 >/dev/null
	pids=
	for i in 1 2 3 4; do sh -c 'while :; do :; done' & pids="$pids $!"; done
	./waker > waker.out & W=$!
	sysctl -w machdep.lpsched.fgpid=$W >/dev/null
	sleep 20
	kill -TERM $W; wait $W; kill $pids; wait 2>/dev/null
	echo "=== pack=$pack"
	cat waker.out
	sysctl -n machdep.lpsched.latency_hist
	sysctl -n machdep.lpsched.stats | tr ' ' '\n' | grep -E 'pack_|catch|exempt_fg' | tr '\n' ' '; echo
done
sysctl -w machdep.lpsched.enabled=0 machdep.lpsched.pack=0 machdep.lpsched.fgpid=0 >/dev/null
