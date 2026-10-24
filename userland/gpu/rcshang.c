/*
 * rcshang: dump the render engine (rcs0) state of a hung Tiger Lake GPU.
 * Maps BAR0 through /dev/mem.  Read-only except forcewake, taken with the
 * USER bit (bit 1, as i915's debugfs does) so the kernel's bit 0 is untouched.
 */
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t *M;
static uint32_t rd(uint32_t o) { return M[o / 4]; }
static void wr(uint32_t o, uint32_t v) { M[o / 4] = v; }
#define MEN(b) (((b) << 16) | (b))
#define MCL(b) ((b) << 16)
#define FW_USER (1u << 1)

int
main(int argc, char **argv)
{
	unsigned long long bar = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x603c000000ULL;
	int fd = open("/dev/mem", O_RDWR), i;
	struct timespec t0, t;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	M = mmap(0, 0x200000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bar);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	printf("forcewake ack before: render=%#x gt=%#x\n", rd(0xd84), rd(0x130044));
	wr(0xa278, MEN(FW_USER)); wr(0xa188, MEN(FW_USER));
	clock_gettime(CLOCK_MONOTONIC, &t0);
	do clock_gettime(CLOCK_MONOTONIC, &t);
	while (!((rd(0xd84) & FW_USER) && (rd(0x130044) & FW_USER)) &&
	    (t.tv_sec - t0.tv_sec) * 1000000000L + t.tv_nsec - t0.tv_nsec < 5000000L);
	printf("forcewake ack after:  render=%#x gt=%#x\n", rd(0xd84), rd(0x130044));

	struct { const char *n; uint32_t o; } r[] = {
		{"RING_TAIL", 0x30}, {"RING_HEAD", 0x34}, {"RING_START", 0x38},
		{"RING_CTL", 0x3c}, {"ACTHD", 0x74}, {"ACTHD_UDW", 0x5c},
		{"BB_ADDR", 0x140}, {"BB_ADDR_UDW", 0x168}, {"BB_STATE", 0x110},
		{"BB_PER_CTX_PTR", 0x1c0}, {"IPEIR", 0x64}, {"IPEHR", 0x68},
		{"INSTDONE", 0x6c}, {"INSTPS", 0x70}, {"EIR", 0xb0}, {"EMR", 0xb4},
		{"ESR", 0xb8}, {"MI_MODE", 0x9c}, {"MODE_GEN7", 0x29c},
		{"RESET_CTL", 0xd0}, {"HWSTAM", 0x98}, {"IMR", 0xa8},
		{"EL_STATUS_LO", 0x234}, {"EL_STATUS_HI", 0x238},
		{"CTX_CTRL", 0x244}, {"CSB_PTR", 0x3a0}, {"PP_DIR_BASE", 0x228},
		{"SEMA_WAIT_POLL", 0x24c}, {"WAIT_FOR_RC6_EXIT", 0xcc},
		{"NOPID", 0x94}, {"DMA_FADD", 0x78}, {"DMA_FADD_UDW", 0x60},
		{"TIMESTAMP", 0x358}, {"CTX_TIMESTAMP", 0x3a8},
	};
	for (i = 0; i < (int)(sizeof r / sizeof r[0]); i++)
		printf("  rcs0 %-18s (0x%04x) = %08x\n", r[i].n, 0x2000 + r[i].o, rd(0x2000 + r[i].o));
	printf("  rcs0 CSB entries (0x2370..):");
	for (i = 0; i < 12; i++) printf(" %08x", rd(0x2370 + 4 * i));
	printf("\n  rcs0 EXECLIST_SQ (0x2510..):");
	for (i = 0; i < 4; i++) printf(" %08x", rd(0x2510 + 4 * i));
	printf("\n");
	struct { const char *n; uint32_t o; } g[] = {
		{"GEN12_RING_FAULT_REG", 0xcec4}, {"GEN12_FAULT_TLB_DATA0", 0xceb8},
		{"GEN12_FAULT_TLB_DATA1", 0xcebc}, {"GEN7_SC_INSTDONE", 0x7100},
		{"GEN7_SAMPLER_INSTDONE", 0xe160}, {"GEN7_ROW_INSTDONE", 0xe164},
		{"GEN12_SC_INSTDONE_EXTRA", 0x7104}, {"GEN12_SC_INSTDONE_EXTRA2", 0x7108},
		{"GEN8_EIR(0x20b0)", 0x20b0}, {"GEN6_RC_CONTROL", 0xa090},
		{"GEN6_RC_STATE", 0xa094}, {"GT_CORE_STATUS", 0x138060},
		{"GEN6_RPNSWREQ", 0xa008}, {"GEN6_RP_CONTROL", 0xa024},
		{"RPSTAT1", 0xa01c}, {"GEN9_PG_ENABLE", 0xa210},
		{"GEN6_GDRST", 0x941c}, {"FORCEWAKE_ACK_RENDER", 0xd84},
		{"GEN8_GTCR", 0x4274}, {"ERROR_GEN6", 0x40a0},
		{"GFX_FLSH_CNTL", 0x101008}, {"GEN11_GFX_MSTR_IRQ", 0x190010},
		{"GEN11_GT_INTR_DW0", 0x190018}, {"GEN11_RCS0_RSVD_INTR_MASK", 0x190090},
		{"VCS0 RING_HEAD", 0x1c0034}, {"VCS0 RING_TAIL", 0x1c0030},
		{"VCS0 ACTHD", 0x1c0074}, {"VCS0 MI_MODE", 0x1c009c},
		{"VECS0 RING_HEAD", 0x1c8034}, {"VECS0 RING_TAIL", 0x1c8030},
		{"BCS0 RING_HEAD", 0x22034}, {"BCS0 RING_TAIL", 0x22030},
	};
	for (i = 0; i < (int)(sizeof g / sizeof g[0]); i++)
		printf("  %-26s (0x%06x) = %08x\n", g[i].n, g[i].o, rd(g[i].o));

	wr(0xa278, MCL(FW_USER)); wr(0xa188, MCL(FW_USER));
	return 0;
}
