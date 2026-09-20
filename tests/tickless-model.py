#!/usr/bin/env python3
"""Deterministic model of the tickless hardclock accounting.

Astra's counterexample: lapic_oneshot() overwrites the timer without
accounting for how far the CURRENT periodic interval has already progressed,
so that time never enters lapic_oneshot_done()'s calculation and the carry
cannot recover it.  Model both algorithms and check the invariant

    credited_ticks == floor(total_elapsed / tval)

TVAL counts == one hardclock tick.
"""
TVAL = 1000          # LAPIC counts per tick
HZ_TICK = TVAL

def run(cycles, fix_entry):
    """cycles: list of (awake_counts, sleep_counts, requested_ticks)."""
    frac = 0          # elapsed-but-uncredited counts
    credited = 0      # hardclock ticks credited
    total = 0         # real counts elapsed
    periodic_pos = 0  # counts into the current periodic interval

    for awake, sleep, req in cycles:
        # --- awake on the periodic tick ---
        # Periodic interrupts that fire while awake credit one tick each.
        pos = periodic_pos + awake
        while pos >= TVAL:
            pos -= TVAL
            credited += 1
        total += awake

        # --- enter idle: lapic_oneshot() overwrites the timer ---
        if fix_entry:
            # capture how far the periodic interval had progressed
            frac += pos
            pos = 0
        # (unfixed: `pos` counts are simply discarded here)

        # --- the one-shot interval ---
        total += sleep
        counted = sleep            # measured from the reprogramming instant
        counted += frac
        elapsed = counted // TVAL
        frac = counted % TVAL
        credited += elapsed

        # restore periodic: starts a fresh interval from now
        periodic_pos = 0
    return credited, total

def check(name, cycles, fix_entry):
    credited, total = run(cycles, fix_entry)
    due = total // TVAL
    lost = due - credited
    print(f"  {name:22s} elapsed={total:8d} due={due:6d} credited={credited:6d} "
          f"lost={lost:5d} ({(credited/due-1)*100:+.3f}%)")
    return lost

# Astra's example: 10ms tick; 3ms awake, then a 40ms one-shot woken after 7ms.
print("Astra's counterexample x100 (awake 0.3 tick, sleep 0.7 tick):")
cyc = [(300, 700, 4)] * 100
a = check("current (no entry fix)", cyc, False)
b = check("with entry capture", cyc, True)

print("\nMixed/irregular workload x1000:")
import random
random.seed(7)
cyc = [(random.randint(0, 2500), random.randint(50, 30000), 0) for _ in range(1000)]
c = check("current (no entry fix)", cyc, False)
d = check("with entry capture", cyc, True)

print("\nRESULT:")
print(f"  unfixed loses {a} and {c} ticks -> accounting defect confirmed")
print(f"  fixed loses   {b} and {d} ticks (<=1 = sub-tick rounding only)")
assert a > 0 and c > 0, "expected the current algorithm to lose time"
assert b <= 1 and d <= 1, "entry capture should be exact to within one tick"
print("  model OK")
