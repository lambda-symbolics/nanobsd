/*
 * bcis-probe STREAM BLKSIZE SECONDS: where is the link position (LPIB) when a
 * buffer completion (BCIS) is raised?  Spin-reads the stream descriptor
 * through /dev/mem (read-only: these registers have no read side effects) and
 * records LPIB modulo the block size each time BCIS is seen set, before the
 * interrupt handler clears it.  Run while the stream plays.  HDA BAR of the
 * X1 Nano (00:31:3) hard-coded.
 */
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	off_t bar = 0x603d1d8000ULL;
	int st = argc > 1 ? atoi(argv[1]) : 7;
	unsigned blk = argc > 2 ? atoi(argv[2]) : 1920;
	int secs = argc > 3 ? atoi(argv[3]) : 4;
	int fd = open("/dev/mem", O_RDONLY);
	volatile uint8_t *m, *s;
	unsigned seen = 0, prev = 0, minoff = ~0u, maxoff = 0, minstep = ~0u;
	unsigned lastlpib = 0, hist[8] = { 0 };
	struct timespec t0, t;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	m = mmap(NULL, 0x4000, PROT_READ, MAP_SHARED, fd, bar);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	s = m + 0x80 + st * 0x20;
	printf("stream %d FIFOS=%u FIFOW=%u CBL=%u\n", st,
	    *(volatile uint16_t *)(s + 0x10), *(volatile uint16_t *)(s + 0x0e),
	    *(volatile uint32_t *)(s + 8));
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		unsigned sts = s[3] & 0x04;
		unsigned lpib = *(volatile uint32_t *)(s + 4);

		if (lpib != lastlpib && lpib > lastlpib && lpib - lastlpib < minstep)
			minstep = lpib - lastlpib;
		lastlpib = lpib;
		if (sts && !prev) {
			/* signed offset from the nearest boundary, in bytes */
			int off = (int)(lpib % blk);
			if (off > (int)blk / 2)
				off -= blk;
			seen++;
			if ((unsigned)(off + 4096) < minoff) minoff = off + 4096;
			if ((unsigned)(off + 4096) > maxoff) maxoff = off + 4096;
			hist[off < -512 ? 0 : off < -256 ? 1 : off < -64 ? 2 :
			    off < 0 ? 3 : off < 64 ? 4 : off < 256 ? 5 :
			    off < 512 ? 6 : 7]++;
		}
		prev = sts;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0.tv_sec >= secs)
			break;
	}
	printf("BCIS caught %u times; LPIB offset from boundary: min %d max %d bytes\n",
	    seen, (int)minoff - 4096, (int)maxoff - 4096);
	printf("histogram  <-512:%u  -512..-256:%u  -256..-64:%u  -64..0:%u  "
	    "0..64:%u  64..256:%u  256..512:%u  >512:%u\n", hist[0], hist[1],
	    hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
	printf("smallest LPIB step seen: %u bytes\n", minstep);
	return 0;
}
