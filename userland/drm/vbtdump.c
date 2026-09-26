/* vbtdump ASLS OUT: copy the Intel OpRegion's VBT to OUT (read-only /dev/mem). */
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void *map(int fd, uint64_t pa, size_t len, size_t *off) {
	uint64_t base = pa & ~0xfffULL; *off = pa - base;
	void *m = mmap(0, len + *off, PROT_READ, MAP_SHARED, fd, base);
	return m == MAP_FAILED ? NULL : m;
}
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: vbtdump ASLS OUT\n"); return 1; }
	uint64_t asls = strtoull(argv[1], 0, 0);
	int fd = open("/dev/mem", O_RDONLY); if (fd < 0) { perror("/dev/mem"); return 1; }
	size_t off; uint8_t *op = map(fd, asls, 0x2000, &off); if (!op) { perror("mmap opregion"); return 1; }
	op += off;
	if (memcmp(op, "IntelGraphicsMem", 16)) { fprintf(stderr, "no OpRegion signature\n"); return 1; }
	uint8_t vmin = op[0x16], vmaj = op[0x17];
	uint64_t rvda; uint32_t rvds; memcpy(&rvda, op + 0x3ba, 8); memcpy(&rvds, op + 0x3c2, 4);
	printf("opregion version %u.%u, mailbox4 sig %.4s, rvda 0x%llx rvds %u\n",
	    vmaj, vmin, (char *)op + 0x400, (unsigned long long)rvda, rvds);
	const uint8_t *vbt; size_t len;
	if (!memcmp(op + 0x400, "$VBT", 4)) {
		vbt = op + 0x400; len = 0x1800;
	} else if (rvda && rvds) {
		uint64_t pa = (vmaj > 2 || (vmaj == 2 && vmin >= 1)) ? asls + rvda : rvda;
		size_t o2; uint8_t *m = map(fd, pa, rvds, &o2); if (!m) { perror("mmap rvda"); return 1; }
		vbt = m + o2; len = rvds;
	} else { fprintf(stderr, "no VBT found\n"); return 1; }
	FILE *f = fopen(argv[2], "wb"); fwrite(vbt, 1, len, f); fclose(f);
	printf("wrote %zu bytes, signature %.20s\n", len, (const char *)vbt);
	return 0;
}
