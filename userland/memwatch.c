/*
 * memwatch: catch stray writes into RAM.  Fills SIZE MB with a pattern that
 * is unique per 8-byte word, then after every run of TRIGGER (a shell
 * command) checks every page and reports pages that changed: how many words,
 * whether the whole page became zero, and the changed range.  The dumps of
 * 2026-10-26..29 show whole kernel pages zeroed after xhci0 came back from a
 * TCSS power loss; a user page hit the same way shows up here.
 *
 *   memwatch SIZE_MB ROUNDS 'TRIGGER'      (TRIGGER "" = just wait 10 s)
 *
 * The pattern is locked into RAM (mlock) so paging cannot touch it.
 * MEMWATCH_ROLL=MB adds a second region that is mapped afresh at the end of
 * every round and checked at the end of the next one: it takes pages from
 * the free list, so a page that something freed but still writes to (a
 * device or GPU mapping left behind) can land in it.
 */
#include <sys/mman.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PG	4096
#define WPP	(PG / 8)

static uint64_t
pat(size_t page, size_t w)
{
	return 0xa5c3000000000000ULL ^ ((uint64_t)page << 9) ^ w;
}

static void
fill(uint64_t *mem, size_t npages)
{
	size_t p, w;

	for (p = 0; p < npages; p++)
		for (w = 0; w < WPP; w++)
			mem[p * WPP + w] = pat(p, w);
}

/* Report and re-arm changed pages; returns the number changed. */
static size_t
check(const char *what, size_t r, uint64_t *mem, size_t npages,
    size_t *zeroedp)
{
	size_t p, w, bad = 0, zeroed = 0;

	for (p = 0; p < npages; p++) {
		size_t nchg = 0, nzero = 0, lo = WPP, hi = 0;

		for (w = 0; w < WPP; w++) {
			uint64_t v = mem[p * WPP + w];

			if (v != pat(p, w)) {
				nchg++;
				if (v == 0)
					nzero++;
				if (w < lo)
					lo = w;
				hi = w;
			}
		}
		if (nchg == 0)
			continue;
		bad++;
		if (nzero == WPP)
			zeroed++;
		if (bad <= 20)
			printf("round %zu %s: page %zu (va %p): %zu words "
			    "changed (%zu zero), bytes %#zx-%#zx, "
			    "first %#llx\n", r, what, p, (void *)&mem[p * WPP],
			    nchg, nzero, lo * 8, hi * 8 + 7,
			    (unsigned long long)mem[p * WPP + lo]);
		for (w = 0; w < WPP; w++)
			mem[p * WPP + w] = pat(p, w);
	}
	*zeroedp = zeroed;
	return bad;
}

int
main(int argc, char **argv)
{
	size_t mb, npages, bad, zeroed, rounds, r, rollpages = 0;
	size_t rbad = 0, rzeroed = 0;
	uint64_t *mem, *roll = NULL;
	const char *e;

	if (argc != 4)
		errx(2, "usage: memwatch SIZE_MB ROUNDS 'TRIGGER'");
	mb = strtoul(argv[1], NULL, 10);
	rounds = strtoul(argv[2], NULL, 10);
	npages = mb * 256;
	mem = mmap(NULL, npages * PG, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap");
	fill(mem, npages);
	if ((e = getenv("MEMWATCH_ROLL")) != NULL)
		rollpages = strtoul(e, NULL, 10) * 256;
	if (mlock(mem, npages * PG) != 0)
		warn("mlock (continuing unlocked)");
	printf("memwatch: %zu MB filled\n", mb);
	fflush(stdout);

	for (r = 1; r <= rounds; r++) {
		time_t t = time(NULL);

		if (argv[3][0] != '\0') {
			if (system(argv[3]) != 0)
				printf("round %zu: trigger failed\n", r);
		} else
			sleep(10);
		sleep(5);
		bad = check("locked", r, mem, npages, &zeroed);
		/* The fresh region lived through this round's trigger. */
		if (roll != NULL) {
			rbad = check("fresh", r, roll, rollpages, &rzeroed);
			munmap(roll, rollpages * PG);
			roll = NULL;
		}
		if (rollpages != 0) {
			roll = mmap(NULL, rollpages * PG, PROT_READ | PROT_WRITE,
			    MAP_ANON | MAP_PRIVATE, -1, 0);
			if (roll == MAP_FAILED)
				err(1, "mmap roll");
			fill(roll, rollpages);
		}
		printf("round %zu (%s): %zu pages changed, %zu wholly zeroed",
		    r, strtok(ctime(&t), "\n"), bad, zeroed);
		if (r > 1 && rollpages != 0)
			printf("; fresh %zu MB: %zu changed, %zu zeroed",
			    rollpages / 256, rbad, rzeroed);
		printf("\n");
		fflush(stdout);
	}
	return 0;
}
