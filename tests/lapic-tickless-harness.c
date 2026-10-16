/*
 * lapic-tickless-harness.c: host test for the tickless LAPIC accounting.
 *
 * Simulates the timer hardware (periodic boundaries, one-shot deadline, the
 * pending bit, the TSC) and feeds the same timeline to the algorithm before
 * the 2026-10-17 change (elapsed inferred from the timer's remaining count and
 * pending bit) and to the current one (kernel/arch/x86/x86/lapic_tl.h: elapsed
 * from the TSC since the entry sample).  The register orders follow
 * lapic_oneshot() and lapic_oneshot_done() exactly.
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -I../kernel/arch/x86/x86 \
 *            -o lapic-tl lapic-tickless-harness.c && ./lapic-tl
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lapic_tl.h"

#define TVAL	384000u		/* 38.4 MHz LAPIC, hz = 100 */
#define LAPIC_HZ 38400000ull
#define TSC_HZ	2112000000ull	/* TSC = 55 * LAPIC count */

/* Hardware: time in LAPIC counts. */
struct hw {
	uint64_t now, next_boundary, deadline;
	int irr, oneshot;
};
static uint64_t tsc(const struct hw *h) { return h->now * (TSC_HZ / LAPIC_HZ); }

/* Per-algorithm state. */
struct alg {
	uint32_t frac;
	uint64_t entry_tsc, credited;
};

enum race { RACE_NONE, RACE_BEFORE_CCR, RACE_AFTER_CCR };
enum wake { WAKE_EARLY, WAKE_EXPIRE, WAKE_EXPIRE_AFTER_READ };

static void
periodic_boundary(struct hw *h)
{
	h->now = h->next_boundary;
	h->irr = 1;
	h->next_boundary += TVAL;
}

/* One tickless episode; returns 0 when the entry was refused. */
static int
episode(struct hw *h, struct alg *old, struct alg *new, uint32_t nticks,
    enum race race, enum wake wake, uint64_t early)
{
	uint32_t ccr, used, ofrac, nfrac;
	uint64_t counted, since, rem;
	unsigned elapsed, manual;
	int pending;

	/* lapic_oneshot: refuse when a timer interrupt is already pending. */
	if (h->irr)
		return 0;
	if (race == RACE_BEFORE_CCR)
		periodic_boundary(h);	/* expires and reloads before the sample */
	/* entry sample: TSC and CCR together */
	new->entry_tsc = tsc(h);
	ccr = (uint32_t)(h->next_boundary - h->now);
	used = ccr < TVAL ? TVAL - ccr : 0;
	old->frac += used;
	new->frac += used;
	if (race == RACE_AFTER_CCR) {
		periodic_boundary(h);	/* expires between the sample and reprogram */
		h->now += 3;
	}
	h->deadline = h->now + (uint64_t)TVAL * nticks;
	h->oneshot = 1;

	/* MWAIT: an already pending interrupt ends it at once. */
	if (h->irr)
		h->now += 5;
	else if (wake == WAKE_EARLY && h->now + early < h->deadline)
		h->now += early;
	else if (wake == WAKE_EXPIRE_AFTER_READ)
		h->now = h->deadline - 2;	/* expires between TSC read and restore */
	else {
		h->now = h->deadline;
		h->irr = 1;
	}

	/* old lapic_oneshot_done: CCR, then IRR, counted from the timer */
	ofrac = old->frac;		/* frac_in */
	rem = h->irr ? 0 : h->deadline - h->now;
	pending = h->irr;
	counted = (uint64_t)TVAL * nticks;
	if (!pending)
		counted = rem <= counted ? counted - rem : 0;
	counted += ofrac;
	elapsed = (unsigned)(counted / TVAL);
	old->frac = (uint32_t)(counted % TVAL);
	/* new: TSC first */
	uint64_t now_tsc = tsc(h);
	if (wake == WAKE_EXPIRE_AFTER_READ && !h->irr) {
		h->now = h->deadline;	/* expiry before the restore */
		h->irr = 1;
	}
	/* old: the re-check after the restore counts the full interval */
	if (!pending && h->irr) {
		pending = 1;
		counted = (uint64_t)TVAL * nticks + ofrac;
		elapsed = (unsigned)(counted / TVAL);
		old->frac = (uint32_t)(counted % TVAL);
	}
	/* restore periodic */
	h->oneshot = 0;
	h->next_boundary = h->now + TVAL;
	/* credit old: manual + interrupt */
	manual = elapsed >= (unsigned)pending ? elapsed - (unsigned)pending : 0;
	old->credited += manual + (unsigned)h->irr;

	/* new: lapic_tl_credit with the final pending state */
	since = lapic_tl_counts(now_tsc - new->entry_tsc, TSC_HZ, LAPIC_HZ);
	elapsed = lapic_tl_credit(new->frac, since, TVAL, h->irr, &nfrac, &manual);
	new->frac = nfrac;
	new->credited += manual + (unsigned)h->irr;

	h->irr = 0;	/* delivered on sti */
	return 1;
}

static void
ticking(struct hw *h, struct alg *old, struct alg *new, unsigned n)
{
	while (n--) {
		periodic_boundary(h);
		old->credited++;
		new->credited++;
		h->irr = 0;
	}
}

static void
start(struct hw *h, struct alg *old, struct alg *new)
{
	*h = (struct hw){ .now = 0, .next_boundary = TVAL };
	*old = (struct alg){0};
	*new = (struct alg){0};
}

static double
drift(const struct hw *h, const struct alg *a)
{
	/* real whole ticks since time 0 (the first boundary is at TVAL) */
	return (double)a->credited - (double)(h->now / TVAL);
}

static int failures;
static void
check(const char *name, double got, double lo, double hi)
{
	int ok = got >= lo && got <= hi;
	printf("%-58s %+8.2f ticks  %s\n", name, got, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

int
main(void)
{
	struct hw h;
	struct alg o, n;

	/* 1. ordinary early wake, mid-interval entry */
	start(&h, &o, &n);
	h.now = TVAL / 3;
	episode(&h, &o, &n, 4, RACE_NONE, WAKE_EARLY, TVAL * 2);
	printf("1 ordinary early wake:     old %+.2f  new %+.2f\n", drift(&h, &o), drift(&h, &n));
	check("1 new", drift(&h, &n), -1, 0);

	/* 2. periodic expiry between the sample and the reprogram (reviewer) */
	start(&h, &o, &n);
	h.now = TVAL - 10;
	episode(&h, &o, &n, 100, RACE_AFTER_CCR, WAKE_EARLY, 0);
	printf("2 race after sample:       old %+.2f  new %+.2f\n", drift(&h, &o), drift(&h, &n));
	check("2 old is expected to over-credit (bug)", drift(&h, &o), 50, 200);
	check("2 new", drift(&h, &n), -1, 0);

	/* 3. periodic expiry and reload before the sample */
	start(&h, &o, &n);
	h.now = TVAL - 10;
	episode(&h, &o, &n, 100, RACE_BEFORE_CCR, WAKE_EARLY, 0);
	printf("3 race before sample:      old %+.2f  new %+.2f\n", drift(&h, &o), drift(&h, &n));
	check("3 new", drift(&h, &n), -1, 0);

	/* 4. one-shot expiry between the TSC read and the restore */
	start(&h, &o, &n);
	h.now = TVAL / 2;
	episode(&h, &o, &n, 10, RACE_NONE, WAKE_EXPIRE_AFTER_READ, 0);
	printf("4 expiry after TSC read:   old %+.2f  new %+.2f\n", drift(&h, &o), drift(&h, &n));
	check("4 new", drift(&h, &n), -1, 0);

	/* 5. long randomized run: ticking, early wakes, expiries, races */
	start(&h, &o, &n);
	srand(12345);
	for (int i = 0; i < 2000000; i++) {
		unsigned r = (unsigned)rand();
		if (r % 7 == 0)
			ticking(&h, &o, &n, 1 + r % 5);
		h.now += (uint64_t)(rand() % (int)(h.next_boundary - h.now));
		enum race race = (r % 1000 == 1) ? RACE_AFTER_CCR :
		    (r % 1000 == 2) ? RACE_BEFORE_CCR : RACE_NONE;
		enum wake wake = (r % 11 == 0) ? WAKE_EXPIRE :
		    (r % 997 == 3) ? WAKE_EXPIRE_AFTER_READ : WAKE_EARLY;
		uint32_t nticks = 4 + (uint32_t)(rand() % 97);
		uint64_t early = (uint64_t)rand() % ((uint64_t)TVAL * nticks);
		if (!episode(&h, &o, &n, nticks, race, wake, early))
			ticking(&h, &o, &n, 0);
	}
	printf("5 random 2e6 episodes (%.0f s): old %+.1f  new %+.1f\n",
	    (double)h.now / LAPIC_HZ, drift(&h, &o), drift(&h, &n));
	check("5 new stays within a tick of real time", drift(&h, &n), -1, 0);
	check("5 old drifts (documents the bug)", drift(&h, &o), 100, 1e9);

	/* 6. conversion: hours of TSC delta do not overflow */
	assert(lapic_tl_counts(TSC_HZ * 36000ull, TSC_HZ, LAPIC_HZ) == LAPIC_HZ * 36000ull);

	printf(failures ? "FAILED (%d)\n" : "all checks passed\n", failures);
	return failures != 0;
}
