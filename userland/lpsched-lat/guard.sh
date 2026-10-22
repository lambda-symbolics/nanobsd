#!/bin/sh
# Sibling guard: a focused busy thread + 3 background periodic wakers, pack=1.
cd "$(dirname "$0")"
cc -O2 -o spin spin.c && cc -O2 -o waker waker.c || exit 1
st() { sysctl -n machdep.lpsched.stats | tr ' ' '\n' | grep -E "^($1)=" | cut -d= -f2; }
sysctl -w machdep.lpsched.enabled=1 machdep.lpsched.pack=1 >/dev/null
for lat in 1 0 1 0; do
	sysctl -w machdep.lpsched.latency=$lat machdep.lpsched.latency_reset=1 >/dev/null
	s0=$(st pack_sibling); g0=$(st pack_guard)
	./spin > spin.out & S=$!
	sysctl -w machdep.lpsched.fgpid=$S >/dev/null
	pids=; for i in 1 2 3; do ./waker > /dev/null & pids="$pids $!"; done
	sleep 20
	kill -TERM $S $pids; wait 2>/dev/null
	echo "=== guard(latency)=$lat: $(cat spin.out) pack_sibling=+$(( $(st pack_sibling) - s0 )) pack_guard=+$(( $(st pack_guard) - g0 ))"
	[ $lat = 1 ] && sysctl -n machdep.lpsched.latency_hist | grep -E '^(fg|user)'
done
sysctl -w machdep.lpsched.enabled=0 machdep.lpsched.pack=0 machdep.lpsched.fgpid=0 machdep.lpsched.latency=1 >/dev/null
