/* Models NetBSD hid_get_item()'s variable-item expansion and nanobsd
 * imt_scan_features()'s last-match selection. No I2C access.
 * Fixture: Microsoft's Surface/Button Switch report: two data bits,
 * followed by six Constant|Variable padding bits without new usages.
 * This is not the Nano's captured descriptor.
 * Build: cc -std=c11 -Wall -Wextra -Werror -O2 FILE.c -o padding-test
 *
 * Origin: written by Astra during review of nanobsd 568f22b.  The defect it
 * demonstrates was real on this machine: the pad's report 5 logged
 *   usage 0x000d0057 pos 0 / 0x000d0058 pos 1   (the actual switches)
 *   usage 0x000d0057 pos 2 / 0x000d0058 pos 3   (padding, stale labels)
 * so imt wrote 0x0c instead of 0x03, left both reporting switches clear,
 * and the touchpad went silent in Precision Touchpad mode while continuing
 * to work in mouse mode.  Fixed by filtering HIO_CONST in imt_scan_features()
 * and imt_parse_input().
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define SURFACE 0x000d0057u
#define BUTTON  0x000d0058u
#define HIO_CONST 1u
struct item { uint32_t usage, pos, size, flags; };
struct parser { uint32_t usages[256]; int nu; unsigned pos; };

static unsigned uimin(unsigned a, unsigned b) { return a < b ? a : b; }

/* NetBSD clears nu after a Main item, but retains the usages array.
 * The original index is uimin(s->multi, s->nu-1). */
static unsigned emit_variable(struct parser *p, unsigned count,
    unsigned size, unsigned flags, struct item *out)
{
    for (unsigned i = 0; i < count; ++i) {
        unsigned idx = uimin(i, (unsigned)(p->nu - 1));
        assert(idx < 256);
        out[i] = (struct item){p->usages[idx], p->pos, size, flags};
        p->pos += size;
    }
    p->nu = 0;
    return count;
}

static void locate(const struct item *items, unsigned n, int skip_constants,
    unsigned *surface, unsigned *button)
{
    *surface = *button = UINT32_MAX;
    for (unsigned i = 0; i < n; ++i) {
        if (skip_constants && (items[i].flags & HIO_CONST)) continue;
        if (items[i].usage == SURFACE) *surface = items[i].pos;
        if (items[i].usage == BUTTON)  *button = items[i].pos;
    }
}

int main(void)
{
    struct parser p = {.usages = {SURFACE, BUTTON}, .nu = 2, .pos = 0};
    struct item items[8];
    unsigned n = emit_variable(&p, 2, 1, 2, items);
    n += emit_variable(&p, 6, 1, 3, items + n);
    unsigned surface, button;
    for (unsigned i = 0; i < n; ++i)
        printf("usage=%08x pos=%u size=%u flags=%u\n", items[i].usage,
            items[i].pos, items[i].size, items[i].flags);
    locate(items, n, 0, &surface, &button);
    assert(surface == 2 && button == 3);
    printf("unfiltered: surface bit %u, button bit %u (wrong)\n", surface, button);
    unsigned payload = (1u << surface) | (1u << button);
    assert(payload == 0x0c);
    printf("zero-initialized payload -> %02x; actual enable bits -> %u/%u\n",
        payload, payload & 1u, (payload >> 1) & 1u);
    locate(items, n, 1, &surface, &button);
    assert(surface == 0 && button == 1);
    printf("HIO_CONST filtered: surface bit %u, button bit %u (correct)\n", surface, button);
    return 0;
}
