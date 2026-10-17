#!/bin/sh
# usbpm-deadlines.sh [-f] [-g] [QUIET_SECONDS]
#
# Acceptance test for the deadline-driven xhci runtime PM (hw.usbpm.stats
# with callouts=/scans=/armed=/deadlines=).  Run on the laptop as root,
# panel on is fine, no Bluetooth stream running.  It never touches Wi-Fi.
#
#   1  usb on pch: every idle device on the PCH controller suspends, then
#      no deadline and no callout is left armed.
#   2  btconfig ubt0 up: the radio parks within autosuspend + slack.
#   3  QUIET_SECONDS (default 60) with everything parked: the callout does
#      not fire, no scan, no task, no resume.
#   4  btconfig ubt0 inquiry wakes the radio (resumes grow, inquiry works),
#      and it parks again within autosuspend + slack afterwards.
#   5  hw.usbpm.autosuspend=0 wakes the idle-suspended devices and arms
#      nothing; writing 5 back re-parks them.
#   6  forced suspend failures (hw.usbpm.inject_suspend=2) on the radio
#      alone (everything parked, then an inquiry wakes only the radio):
#      failed += 2, retries >= 2, re-park ~4 periods after the inquiry
#      (P, then P and 2P of backoff), nothing armed afterwards.
#   7  (-f) forced resume failures (hw.usbpm.inject_resume=2) during an
#      inquiry: retried after 200/400 ms, the inquiry still works.
#   8  (-g) give-up: inject_suspend=5 on the radio alone: after the
#      bounded backoff (~16 periods) giveups+1 and a slow re-check stays
#      armed; a hw.usbpm.autosuspend write re-arms it and it parks.
#      Skipped (inconclusive) without a give-up.  Takes ~1.5 minutes.
#
# The previous state (autosuspend, debug, radio up/down, PCH on/off) is
# restored at the end.  Log: /var/tmp/usbpm-deadlines.txt (+ dmesg excerpt).
# Exit status: number of failed checks.
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/pkg/sbin:/usr/pkg/bin:/usr/local/sbin:/usr/local/bin
export PATH

FORCE_RESUME=0; GIVEUP=0
while getopts fg o; do
	case $o in
	f) FORCE_RESUME=1 ;;
	g) GIVEUP=1 ;;
	*) echo "usage: $0 [-f] [-g] [QUIET_SECONDS]" >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
QUIET=${1:-60}
AS=5			# hw.usbpm.autosuspend for the test
SLACK=4			# deferral (cidle maxskip 1 s) + task + U3 transition
DEV=ubt0
OUT=/var/tmp/usbpm-deadlines.txt
FAILS=0

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
sysctl -n hw.usbpm.stats 2>/dev/null | grep -q 'callouts=' ||
	{ echo "this kernel has no deadline-driven usbpm (hw.usbpm.stats lacks callouts=)" >&2; exit 2; }
pgrep -x bta2dpd >/dev/null &&
	{ echo "a Bluetooth stream is running (bta2dpd): stop it first (bt-headphones off)" >&2; exit 2; }

log() { echo "$*" | tee -a $OUT; }
now() { date +%s; }
stats() { sysctl -n hw.usbpm.stats; }
# get KEY "STATS"
get() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
pass() { log "PASS: $*"; }
fail() { log "FAIL: $*"; FAILS=$((FAILS + 1)); }
radio_up() { btconfig $DEV 2>/dev/null | grep -q UP; }

# wait_parked BASE_SUSPENDS DORMANT_MAX TIMEOUT: suspends grew, nothing armed,
# no more than DORMANT_MAX dormant slots.  Sets ELAPSED (seconds).
wait_parked() {
	_b=$1; _d=$2; _to=$3; _t0=$(now)
	while :; do
		_s=$(stats)
		if [ "$(get suspends "$_s")" -gt "$_b" ] &&
		    [ "$(get deadlines "$_s")" = 0 ] && [ "$(get armed "$_s")" = 0 ] &&
		    [ "$(get dormant "$_s")" -le "$_d" ]; then
			ELAPSED=$(( $(now) - _t0 )); return 0
		fi
		if [ $(( $(now) - _t0 )) -ge "$_to" ]; then
			ELAPSED=$_to; log "   timeout, stats: $_s"; return 1
		fi
		sleep 0.5
	done
}
# wait_idle TIMEOUT: nothing armed (no requirement on suspends).
wait_idle() {
	_to=$1; _t0=$(now)
	while :; do
		_s=$(stats)
		[ "$(get deadlines "$_s")" = 0 ] && [ "$(get armed "$_s")" = 0 ] &&
			{ ELAPSED=$(( $(now) - _t0 )); return 0; }
		[ $(( $(now) - _t0 )) -ge "$_to" ] && { ELAPSED=$_to; log "   timeout, stats: $_s"; return 1; }
		sleep 0.5
	done
}

# --- remember the state to restore ---------------------------------------
OLD_AS=$(sysctl -n hw.usbpm.autosuspend)
OLD_DEBUG=$(sysctl -n hw.usbpm.debug)
OLD_DEFER=$(sysctl -n hw.usbpm.deferrable)
OLD_PCH=off; usb status 2>/dev/null | grep -q 'pch on' && OLD_PCH=on
OLD_RADIO=down; radio_up && OLD_RADIO=up
restore() {
	sysctl -w hw.usbpm.inject_suspend=0 hw.usbpm.inject_resume=0 >/dev/null
	sysctl -w hw.usbpm.debug=$OLD_DEBUG hw.usbpm.deferrable=$OLD_DEFER >/dev/null
	sysctl -w hw.usbpm.autosuspend=$OLD_AS >/dev/null
	[ $OLD_RADIO = down ] && radio_up && btconfig $DEV down
	[ $OLD_PCH = off ] && usb off pch >/dev/null 2>&1
	log "-- restored: autosuspend=$OLD_AS radio=$OLD_RADIO pch=$OLD_PCH; $(usb status)"
}
trap 'restore; exit 130' INT TERM

log "=== $(date '+%F %T') usbpm-deadlines kernel $(sysctl -n machdep.booted_kernel 2>/dev/null) quiet=${QUIET}s"
log "-- before: autosuspend=$OLD_AS pch=$OLD_PCH radio=$OLD_RADIO; $(stats)"
dmesg_mark=$(dmesg | wc -l)
sysctl -w hw.usbpm.debug=1 hw.usbpm.deferrable=1 >/dev/null
sysctl -w hw.usbpm.inject_suspend=0 hw.usbpm.inject_resume=0 >/dev/null
sysctl -w hw.usbpm.autosuspend=$AS >/dev/null

# --- 1: PCH controller on, idle devices suspend, nothing armed -----------
log "-- 1: usb on pch"
s=$(stats); b=$(get suspends "$s")
usb on pch | tee -a $OUT
if [ $OLD_PCH = on ]; then
	wait_idle $((AS + SLACK)) && pass "1: PCH already on, nothing armed after ${ELAPSED}s" ||
		fail "1: deadlines/callout still armed after $((AS + SLACK))s"
elif wait_parked "$b" 99 $((AS + SLACK)); then
	pass "1: idle PCH devices suspended, nothing armed after ${ELAPSED}s"
else
	fail "1: PCH devices not all parked within $((AS + SLACK))s"
fi
log "   $(stats)"

# --- 2: radio up, parks within autosuspend + slack ------------------------
log "-- 2: btconfig $DEV up"
s=$(stats); b=$(get suspends "$s"); d0=$(get dormant "$s")
if ! radio_up; then
	btconfig $DEV up 2>&1 | tee -a $OUT
fi
radio_up || { fail "2: $DEV did not come up (firmware? dmesg)"; restore; exit $FAILS; }
if wait_parked "$b" "$d0" $((AS + SLACK)); then
	pass "2: radio up+idle parked after ${ELAPSED}s (limit $((AS + SLACK))s)"
else
	fail "2: radio not parked within $((AS + SLACK))s (dormant? see dmesg 'port N:' lines)"
fi
log "   $(stats)"

# --- 3: quiet window: no callout, no scan, no task -----------------------
log "-- 3: quiet ${QUIET}s"
s1=$(stats); sleep "$QUIET"; s2=$(stats)
log "   before: $s1"
log "   after:  $s2"
ok=1
for k in callouts scans tasks resumes suspends reconciles; do
	[ "$(get $k "$s1")" = "$(get $k "$s2")" ] || { ok=0; log "   $k: $(get $k "$s1") -> $(get $k "$s2")"; }
done
[ "$(get armed "$s2")" = 0 ] && [ "$(get deadlines "$s2")" = 0 ] || ok=0
[ $ok = 1 ] && pass "3: ${QUIET}s parked: no callout, no scan, no task, nothing armed" ||
	fail "3: runtime PM ran while everything was parked"

# --- 4: inquiry wakes the radio, then it parks again ----------------------
log "-- 4: btconfig $DEV inquiry"
s=$(stats); r=$(get resumes "$s")
if btconfig $DEV inquiry >>$OUT 2>&1; then
	s=$(stats)
	[ "$(get resumes "$s")" -gt "$r" ] && pass "4: inquiry worked and woke the radio (resumes $r -> $(get resumes "$s"))" ||
		fail "4: inquiry worked but no runtime resume was counted"
else
	fail "4: btconfig inquiry failed"
fi
s=$(stats); b=$(get suspends "$s")
wait_parked "$b" "$d0" $((AS + SLACK)) && pass "4: re-parked ${ELAPSED}s after the inquiry" ||
	fail "4: not re-parked within $((AS + SLACK))s after the inquiry"

# --- 5: autosuspend=0 wakes, arms nothing; 5 re-parks --------------------
log "-- 5: hw.usbpm.autosuspend=0"
s=$(stats); r=$(get resumes "$s")
sysctl -w hw.usbpm.autosuspend=0 >/dev/null
sleep 2
s=$(stats)
if [ "$(get resumes "$s")" -gt "$r" ] && [ "$(get deadlines "$s")" = 0 ] && [ "$(get armed "$s")" = 0 ]; then
	pass "5: autosuspend=0 woke the devices (resumes $r -> $(get resumes "$s")), nothing armed"
else
	fail "5: autosuspend=0: $s"
fi
s1=$(stats); sleep 10; s2=$(stats)
[ "$(get callouts "$s1")" = "$(get callouts "$s2")" ] && [ "$(get suspends "$s1")" = "$(get suspends "$s2")" ] &&
	pass "5: disabled for 10s: no callout, no suspend" || fail "5: runtime PM active while disabled: $s2"
b=$(get suspends "$s2")
sysctl -w hw.usbpm.autosuspend=$AS >/dev/null
wait_parked "$b" "$d0" $((AS + SLACK)) && pass "5: autosuspend=$AS re-parked after ${ELAPSED}s" ||
	fail "5: not re-parked within $((AS + SLACK))s after autosuspend=$AS"

# --- 6: forced suspend failures retry with backoff ------------------------
# The injections are global: park everything first, so that the radio,
# woken alone by an inquiry, takes both.  After the inquiry ends (E): idle
# at E+P fails, retry after P fails, retry after 2P parks: about E+4P.
log "-- 6: hw.usbpm.inject_suspend=2, radio only"
if ! wait_idle $((AS + SLACK)); then
	fail "6: not everything parked before the injection"
else
	s=$(stats); f=$(get failed "$s"); rt=$(get retries "$s"); g=$(get giveups "$s")
	sysctl -w hw.usbpm.inject_suspend=2 >/dev/null
	btconfig $DEV inquiry >>$OUT 2>&1 || log "   (inquiry failed)"
	s=$(stats); b=$(get suspends "$s"); t0=$(now)
	LIMIT=$((4 * AS + SLACK + 2))
	wait_parked "$b" "$d0" $LIMIT; el=$(( $(now) - t0 ))
	s=$(stats)
	log "   $s"
	if [ "$(get failed "$s")" -ge $((f + 2)) ] && [ "$(get retries "$s")" -ge $((rt + 2)) ] &&
	    [ "$(get giveups "$s")" = "$g" ] && [ "$(get deadlines "$s")" = 0 ] && [ "$(get armed "$s")" = 0 ] &&
	    [ "$(get suspends "$s")" -gt "$b" ] && [ $el -ge $((3 * AS)) ] && [ $el -le $LIMIT ]; then
		pass "6: 2 injected failures retried with backoff, radio parked ${el}s after the inquiry (expected ~$((4 * AS))s)"
	else
		fail "6: backoff: failed $f -> $(get failed "$s"), retries $rt -> $(get retries "$s"), ${el}s (expected $((3 * AS))..${LIMIT}s)"
	fi
	[ "$(sysctl -n hw.usbpm.inject_suspend)" = 0 ] || fail "6: injections left unused: $(sysctl -n hw.usbpm.inject_suspend)"
fi
sysctl -w hw.usbpm.inject_suspend=0 >/dev/null

# --- 7 (-f): forced resume failures during an inquiry ---------------------
if [ $FORCE_RESUME = 1 ]; then
	log "-- 7: hw.usbpm.inject_resume=2 + inquiry"
	s=$(stats); f=$(get failed "$s"); rt=$(get retries "$s"); r=$(get resumes "$s")
	sysctl -w hw.usbpm.inject_resume=2 >/dev/null
	if btconfig $DEV inquiry >>$OUT 2>&1; then
		s=$(stats)
		[ "$(get failed "$s")" -ge $((f + 2)) ] && [ "$(get retries "$s")" -ge $((rt + 2)) ] &&
		    [ "$(get resumes "$s")" -gt "$r" ] &&
			pass "7: resume retried after injected failures, inquiry worked" ||
			fail "7: $s"
	else
		fail "7: inquiry failed with injected resume failures"
	fi
	sysctl -w hw.usbpm.inject_resume=0 >/dev/null
	s=$(stats); b=$(get suspends "$s")
	wait_parked "$b" "$d0" $((AS + SLACK)) && pass "7: re-parked after ${ELAPSED}s" ||
		fail "7: not re-parked after the forced resume failures"
fi

# --- 8 (-g): repeated failures end in a slow re-check; a sysctl write re-arms
# Radio alone again (inquiry), 5 injections: failures at E+P, +P, +2P, +4P,
# +8P; the 5th counts as a give-up and leaves a deferrable re-check at 8P.
if [ $GIVEUP = 1 ]; then
	log "-- 8: hw.usbpm.inject_suspend=5, radio only (give-up)"
	if ! wait_idle $((AS + SLACK)); then
		fail "8: not everything parked before the injection"
	else
		s=$(stats); g=$(get giveups "$s")
		sysctl -w hw.usbpm.inject_suspend=5 >/dev/null
		btconfig $DEV inquiry >>$OUT 2>&1 || log "   (inquiry failed)"
		t0=$(now); TO=$((16 * AS + SLACK + 5))
		while [ "$(get giveups "$(stats)")" = "$g" ] && [ $(( $(now) - t0 )) -lt $TO ]; do sleep 1; done
		s=$(stats); el=$(( $(now) - t0 )); log "   ${el}s: $s"
		if [ "$(get giveups "$s")" -gt "$g" ]; then
			if [ "$(get deadlines "$s")" -ge 1 ] && [ "$(get armed "$s")" -ge 1 ]; then
				pass "8: give-up after ${el}s (~$((16 * AS))s expected), slow re-check armed"
			else
				fail "8: give-up without a re-check deadline"
			fi
			b=$(get suspends "$s")
			sysctl -w hw.usbpm.autosuspend=$AS >/dev/null
			wait_parked "$b" "$d0" $((AS + SLACK)) && pass "8: sysctl write re-armed it, parked after ${ELAPSED}s" ||
				fail "8: not parked within $((AS + SLACK))s after the sysctl write"
		else
			log "   (no give-up within ${TO}s: inconclusive, re-arm check skipped)"
		fi
	fi
	sysctl -w hw.usbpm.inject_suspend=0 >/dev/null
	wait_idle $((8 * AS + SLACK)) || true
fi

log "-- dmesg (hw.usbpm.debug lines and xhci messages since the start):"
dmesg | sed -n "$((dmesg_mark + 1)),\$p" | grep -E 'xhci|ubt' | tail -60 >> $OUT
restore
log "=== $(date '+%F %T') done: $FAILS failed check(s)"
exit $FAILS
