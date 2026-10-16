/*	LispBSD: tickless LAPIC accounting arithmetic, shared by lapic.c and
 *	tests/lapic-tickless-harness.c (so the host test runs the kernel's code).
 *
 *	The elapsed time of a tickless sleep is measured with the TSC, which runs
 *	independently of the LAPIC timer's mode and interrupt state.  The timer's
 *	pending bit only decides who delivers the last tick: if a timer interrupt
 *	is pending it will run hardclock once by itself, so the caller replays
 *	one tick fewer.  Before this, the elapsed time was inferred from the
 *	timer (remaining count, or "the whole requested skip" when the interrupt
 *	was pending), and a periodic interrupt that became pending between the
 *	entry checks and the reprogramming was taken for the one-shot's expiry:
 *	the whole skip (up to maxskip ticks) was credited for a sleep that MWAIT
 *	ended at once.
 */
#ifndef _X86_LAPIC_TL_H_
#define _X86_LAPIC_TL_H_

/*
 * TSC cycles -> LAPIC timer counts (at the timer's DIV1 rate), rounded down.
 * Splits the product so that hours of TSC delta cannot overflow 64 bits.
 */
static inline uint64_t
lapic_tl_counts(uint64_t tsc_delta, uint64_t tsc_hz, uint64_t lapic_hz)
{
	uint64_t whole, rest;

	if (tsc_hz == 0)
		return 0;
	whole = tsc_delta / tsc_hz;		/* whole seconds */
	rest = tsc_delta % tsc_hz;		/* < tsc_hz, so rest * lapic_hz fits */
	return whole * lapic_hz + rest * lapic_hz / tsc_hz;
}

/*
 * Credit a tickless sleep.  CARRIED is the uncredited time before the sleep
 * (the sub-tick remainder of earlier sleeps plus the part of the periodic
 * interval that had elapsed at entry), SINCE the time measured since entry,
 * both in timer counts.  PENDING is whether a timer interrupt is pending
 * after the periodic timer was restored.  Returns the whole ticks elapsed,
 * the remainder to carry (*FRAC), and how many the caller replays itself
 * (*MANUAL; the pending interrupt delivers the rest).
 *
 * A pending interrupt with less than one tick counted is a periodic expiry
 * that reloaded the timer before the entry sample (its tick is not in
 * CARRIED; the interrupt credits it), unless rounding left the count a hair
 * short of a tick, in which case that tick is the one the interrupt brings.
 */
#define	LAPIC_TL_SLOP(tval)	((tval) / 1024 + 1)

static inline unsigned
lapic_tl_credit(uint64_t carried, uint64_t since, uint32_t tval, int pending,
    uint32_t *frac, unsigned *manual)
{
	uint64_t counted = carried + since;
	unsigned elapsed;

	if (pending && counted < tval && counted + LAPIC_TL_SLOP(tval) >= tval)
		counted = tval;			/* rounding: the pending tick */
	elapsed = (unsigned)(counted / tval);
	*frac = (uint32_t)(counted % tval);
	*manual = (elapsed > (unsigned)pending) ? elapsed - (unsigned)pending : 0;
	return elapsed;
}

#endif /* _X86_LAPIC_TL_H_ */
