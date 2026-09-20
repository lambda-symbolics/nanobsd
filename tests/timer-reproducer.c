/* Register-level reproducer for nanobsd 7bfb02d.
 * Models the committed lapic_oneshot/oneshot_done arithmetic and the caller's
 * tick replay. No privileged instructions are used; this is not a hardware test.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -o timer-test FILE.c
 *
 * Supplied by the reviewer (Astra) against 7bfb02d.  Two of the three cases
 * are EXPECTED TO FAIL against the current kernel: they are carried here as
 * regression tests for the interrupt-ordering repair that is still owed.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct timer {
    uint32_t tval, ccr, frac;
    unsigned irr;
};

static void arm(struct timer *t, uint32_t nticks)
{
    uint32_t used = t->ccr < t->tval ? t->tval - t->ccr : 0;
    uint64_t count;
    t->frac += used;
    count = (uint64_t)t->tval * nticks;
    if (count == 0) count = 1;
    if (count > UINT32_MAX) count = UINT32_MAX;
    t->ccr = (uint32_t)count;
    /* Reprogramming a timer does not service an already-pending IRQ. */
}

static unsigned finish(struct timer *t, uint32_t nticks, int expire_after_irr,
                       unsigned *pending)
{
    uint64_t counted = (uint64_t)t->tval * nticks;
    unsigned elapsed;
    *pending = t->irr; /* lapic_oneshot_done reads IRR before CCR. */
    if (expire_after_irr) {
        t->irr = 1;
        t->ccr = 0;
    }
    if (!*pending)
        counted = (uint64_t)t->ccr <= counted ? counted - t->ccr : 0;
    counted += t->frac;
    elapsed = (unsigned)(counted / t->tval);
    t->frac = (uint32_t)(counted % t->tval);
    t->ccr = t->tval;
    return elapsed;
}

static unsigned delivered(struct timer *t, unsigned elapsed, unsigned pending)
{
    /* cidle manually replays elapsed-pending; the actual pending timer IRQ
     * subsequently enters lapic_clockintr and adds one hardclock tick. */
    unsigned manual = elapsed >= pending ? elapsed - pending : 0;
    unsigned credits = manual + t->irr;
    t->irr = 0;
    return credits;
}

int main(void)
{
    unsigned pending, elapsed, credit;
    /* One tick is 1000 counts; at hz=100 it represents 10 ms. */
    struct timer t = {1000, 700, 0, 0};
    arm(&t, 4);              /* Capture 0.3 ticks of periodic progress. */
    t.ccr -= 700;            /* Then 0.7 ticks asleep, early non-timer wake. */
    elapsed = finish(&t, 4, 0, &pending);
    credit = delivered(&t, elapsed, pending);
    assert(credit == 1 && t.frac == 0);
    printf("ordinary entry+early wake: expected 1 tick, credited %u (PASS)\n", credit);

    /* Periodic timer expired with interrupts disabled, reloaded, and ran
     * 10 more counts. One whole tick is pending, not serviced. */
    t = (struct timer){1000, 990, 0, 1};
    arm(&t, 4);
    /* MWAIT breaks immediately because the old timer IRQ is pending. */
    elapsed = finish(&t, 4, 0, &pending);
    credit = delivered(&t, elapsed, pending);
    assert(credit == 4); /* Current algorithm's erroneous result. */
    printf("old IRQ pending at entry: expected 1 tick, credited %u (BUG)\n", credit);

    t = (struct timer){1000, 1000, 0, 0};
    arm(&t, 4);
    t.ccr = 1; /* External wake just before the one-shot reaches zero. */
    elapsed = finish(&t, 4, 1, &pending);
    credit = delivered(&t, elapsed, pending);
    assert(credit == 5);
    printf("expiry between IRR/CCR reads: expected 4 ticks, credited %u (BUG)\n", credit);

    puts("The two failures are constructed register schedules, not measured frequencies.");
    return 0;
}
