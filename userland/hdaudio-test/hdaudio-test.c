/*
 * hdaudio-test: host test of the LispBSD HD audio command transport and
 * stream completion accounting (kernel/dev/hdaudio/hdaudio.c, 2026-10-21).
 *
 * The functions under test are extracted verbatim from the kernel source by
 * extract.sh and compiled against the mocks below: a codec that answers each
 * command after a scripted delay (or never), unsolicited responses injected at
 * chosen times, and a stream whose link position (LPIB) and clock the test
 * sets.  Run: ./run.sh [path/to/hdaudio.c]
 *
 * The scenarios are the ones Astra's 2026-10-19 review demonstrated against
 * the old code (late reply taken by the next command, nested command taking
 * the outer reply, error interrupt counted as a completion, late interrupt
 * leaving the audio ring a block off the DMA engine), plus lost whole rings.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef unsigned int u_int;

/* --- mocked kernel environment ------------------------------------------ */

#define KASSERT(x)		assert(x)
#define mutex_owned(m)		1
#define BUS_DMASYNC_POSTREAD	0
#define BUS_DMASYNC_PREREAD	0
#define BUS_DMASYNC_POSTWRITE	0
#define BUS_DMASYNC_PREWRITE	0
#define bus_dmamap_sync(...)	do { } while (0)
#define hda_trace(...)		do { } while (0)
#define hda_error(sc, ...)	(errors++, (verbose ? printf("    kernel: " __VA_ARGS__) : 0))

#define HDAUDIO_MMIO_CORBWP	0x48
#define HDAUDIO_MMIO_CORBRP	0x4a
#define HDAUDIO_MMIO_RIRBWP	0x58
#define HDAUDIO_MMIO_RIRBSTS	0x5d
#define HDAUDIO_SD_LPIB(x)	(0x1000 + (x))
#define HDAUDIO_STS_DESE	(1 << 4)
#define HDAUDIO_STS_FIFOE	(1 << 3)
#define HDAUDIO_STS_BCIS	(1 << 2)
#define HDAUDIO_UNSOLQ_LEN	32
#define HDAUDIO_CMD_TIMEOUT	500
#define HDAUDIO_CMD_SETTLE	100

struct rirb_entry {
	uint32_t resp;
	uint32_t resp_ex;
};
#define RIRB_CODEC_ID(e)	((e)->resp_ex & 0xf)
#define RIRB_UNSOL(e)		((e)->resp_ex & 0x10)

struct hdaudio_dma {
	void *kaddr;
	size_t dma_size;
	int dma_map;
};
#define DMA_KERNADDR(d)		((d)->kaddr)

struct hdaudio_softc {
	int sc_corb_mtx;
	int sc_dmat;
	struct hdaudio_dma sc_corb, sc_rirb;
	uint16_t sc_rirbrp;
	bool sc_rirb_desync;
	struct {
		uint32_t resp, resp_ex;
	} sc_unsolq[HDAUDIO_UNSOLQ_LEN];
	u_int sc_unsolq_head, sc_unsolq_tail;
	void *sc_unsol_sih;
	struct timeval sc_stream_lastlog;
};

struct hdaudio_codec {
	struct hdaudio_softc *co_host;
	u_int co_addr;
};

struct hdaudio_stream {
	struct hdaudio_softc *st_host;
	int st_shift, st_num;
	u_int st_blksize, st_nblk, st_curblk;
	uint64_t st_blkns, st_lastns;
};

static struct {
	uint64_t cmd_timeouts, stale_replies, foreign_replies, unsol_events,
	    unsol_dropped, stream_irqs, stream_late, stream_early, stream_laps,
	    stream_fifoe, stream_dese, stream_spurious;
} hdaudio_stats;
#define HDAUDIO_STAT_INC(f)	(hdaudio_stats.f++)
#define HDAUDIO_STAT_ADD(f, n)	(hdaudio_stats.f += (n))

static int errors, softints, verbose;

/* Mock clock (ns) and hardware. */
static uint64_t now_ns;
static uint32_t corb[256];
static uint64_t rirb[256];
static uint16_t corbwp, rirbwp;
static uint32_t lpib;

struct pending {
	uint64_t when;
	struct rirb_entry e;
	bool used;
};
static struct pending pend[64];
static int ncmd;			/* commands the codec has seen */
static long reply_delay_us[64];		/* per command index; <0: never */

static void
deliver(uint64_t when, uint32_t resp, uint32_t resp_ex)
{
	for (int i = 0; i < 64; i++)
		if (!pend[i].used) {
			pend[i] = (struct pending){ when, { resp, resp_ex }, true };
			return;
		}
	abort();
}

static void
run_hardware(void)
{
	for (;;) {
		int best = -1;
		for (int i = 0; i < 64; i++)
			if (pend[i].used && pend[i].when <= now_ns &&
			    (best < 0 || pend[i].when < pend[best].when))
				best = i;
		if (best < 0)
			return;
		rirbwp = (rirbwp + 1) & 0xff;
		memcpy(&rirb[rirbwp], &pend[best].e, sizeof(pend[best].e));
		pend[best].used = false;
	}
}

static void
hda_delay(int us)
{
	now_ns += (uint64_t)us * 1000;
	run_hardware();
}

static uint16_t
hda_read2(struct hdaudio_softc *sc, int reg)
{
	(void)sc;
	run_hardware();
	switch (reg) {
	case HDAUDIO_MMIO_CORBWP: return corbwp;
	case HDAUDIO_MMIO_CORBRP: return corbwp;
	case HDAUDIO_MMIO_RIRBWP: return rirbwp;
	}
	return 0;
}

static uint8_t
hda_read1(struct hdaudio_softc *sc, int reg)
{
	(void)sc; (void)reg;
	return 0;
}

static uint32_t
hda_read4(struct hdaudio_softc *sc, int reg)
{
	(void)sc;
	assert(reg >= 0x1000);
	return lpib;
}

static void
hda_write1(struct hdaudio_softc *sc, int reg, uint8_t v)
{
	(void)sc; (void)reg; (void)v;
}

/* Writing CORBWP hands the codec a command: schedule its reply. */
static void
hda_write2(struct hdaudio_softc *sc, int reg, uint16_t v)
{
	(void)sc;
	if (reg != HDAUDIO_MMIO_CORBWP)
		return;
	corbwp = v;
	uint32_t verb = corb[v];
	int idx = ncmd++;
	if (reply_delay_us[idx] >= 0)
		deliver(now_ns + (uint64_t)reply_delay_us[idx] * 1000,
		    0x1000 + idx, verb >> 28);
}

static void
softint_schedule(void *sih)
{
	(void)sih;
	softints++;
}

static int
ratecheck(struct timeval *last, const struct timeval *rate)
{
	(void)last; (void)rate;
	return 1;
}

static void
nanouptime(struct timespec *ts)
{
	ts->tv_sec = now_ns / 1000000000ULL;
	ts->tv_nsec = now_ns % 1000000000ULL;
}

#include "extracted.inc"

/* --- tests ---------------------------------------------------------------- */

static int failures;

#define CHECK(cond, ...) do {						\
	if (cond) printf("ok   " __VA_ARGS__);				\
	else { printf("FAIL " __VA_ARGS__); failures++; }		\
	printf("\n");							\
} while (0)

static struct hdaudio_softc sc;
static struct hdaudio_codec codec0 = { &sc, 0 }, codec2 = { &sc, 2 };

static void
reset_transport(void)
{
	memset(&sc, 0, sizeof(sc));
	sc.sc_corb = (struct hdaudio_dma){ corb, sizeof(corb), 0 };
	sc.sc_rirb = (struct hdaudio_dma){ rirb, sizeof(rirb), 0 };
	sc.sc_unsol_sih = &sc;
	memset(pend, 0, sizeof(pend));
	memset(&hdaudio_stats, 0, sizeof(hdaudio_stats));
	corbwp = rirbwp = 0;
	ncmd = 0;
	for (int i = 0; i < 64; i++)
		reply_delay_us[i] = 30;		/* a normal codec */
	softints = errors = 0;
}

/* The reply to command index i carries 0x1000 + i. */
static void
test_transport(void)
{
	uint32_t r;
	int err;

	printf("command transport\n");

	reset_transport();
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1000, "normal command gets its reply (0x%x)", r);

	/* A's reply comes 20 ms late: after A gave up and after B finished. */
	reset_transport();
	reply_delay_us[0] = 20000;
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == ETIMEDOUT && r == 0xffffffff,
	    "command without a reply times out (err %d)", err);
	CHECK(now_ns <= 6000000, "and gives up within 5 ms (%.1f ms), "
	    "not the old 50 ms", now_ns / 1e6);
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1001, "next command B gets B's reply (0x%x)", r);
	now_ns = 25000000;
	run_hardware();
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1002, "after A's late reply landed, C gets "
	    "C's reply (0x%x), not A's", r);
	CHECK(hdaudio_stats.stale_replies == 1,
	    "A's late reply dropped as stale (%ju)",
	    (uintmax_t)hdaudio_stats.stale_replies);

	/* A's reply lands just after the timeout: B's settle window takes it. */
	reset_transport();
	reply_delay_us[0] = 5300;
	(void)hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1001, "reply landing in the settle window "
	    "is dropped, B gets its own (0x%x)", r);

	/* Late reply from another codec arrives while B waits. */
	reset_transport();
	reply_delay_us[0] = 6400;	/* codec 2's A */
	reply_delay_us[1] = 1000;	/* codec 0's B, 1 ms */
	(void)hdaudio_command_locked(&codec2, 0x03, 0xf09, 0, &r);
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1001 && hdaudio_stats.foreign_replies == 1,
	    "another codec's late reply during B's wait is dropped (0x%x, "
	    "foreign %ju)", r, (uintmax_t)hdaudio_stats.foreign_replies);

	/* An unsolicited response arrives while a command waits. */
	reset_transport();
	deliver(now_ns + 10000, 0x08000000, 0x10 | 2);	/* tag 2 from codec 2 */
	err = hdaudio_command_locked(&codec0, 0x14, 0xf09, 0, &r);
	CHECK(err == 0 && r == 0x1000, "command gets its reply past an "
	    "unsolicited response (0x%x)", r);
	CHECK(sc.sc_unsolq_tail - sc.sc_unsolq_head == 1 && softints == 1,
	    "the unsolicited response is queued for the soft interrupt, not "
	    "run inside the command");
	CHECK(sc.sc_unsolq[0].resp >> 26 == 2,
	    "and keeps its full payload: tag %u", sc.sc_unsolq[0].resp >> 26);

	/* The interrupt handler's drain: unsolicited queued, nothing waited. */
	reset_transport();
	deliver(now_ns, 0x04000000, 0x10);
	run_hardware();
	{
		uint64_t before = now_ns;
		bool pending = hdaudio_rirb_drain(&sc);

		CHECK(pending && now_ns == before,
		    "interrupt-side drain queues without waiting");
	}
}

/*
 * Stream accounting.  Ring of 3 blocks of 10 ms (1920 bytes at 48 kHz,
 * 16-bit stereo), started at t = 0 in block 0.
 */
#define BLK	1920u
#define BLKNS	10000000ull

static struct hdaudio_stream st;

static void
stream_start(void)
{
	memset(&st, 0, sizeof(st));
	st.st_host = &sc;
	st.st_num = 7;
	st.st_blksize = BLK;
	st.st_nblk = 3;
	st.st_blkns = BLKNS;
	st.st_lastns = now_ns = 0;
	memset(&hdaudio_stats, 0, sizeof(hdaudio_stats));
}

/* The DMA position at time t, with the controller this far ahead. */
static uint32_t
lpib_at(uint64_t t)
{
	return (uint32_t)((t * BLK / BLKNS) % (3 * BLK));
}

static u_int
irq_at(uint64_t t, uint8_t sts)
{
	now_ns = t;
	lpib = lpib_at(t);
	return hdaudio_stream_elapsed(&st, sts);
}

static void
test_stream(void)
{
	u_int n;

	printf("stream accounting\n");

	stream_start();
	n = irq_at(10 * 1000000 + 20000, HDAUDIO_STS_BCIS);
	CHECK(n == 1 && st.st_curblk == 1, "on-time completion: 1 block");

	n = irq_at(20 * 1000000 - 30000, HDAUDIO_STS_BCIS);
	CHECK(n == 1 && st.st_curblk == 2 && hdaudio_stats.stream_early == 1,
	    "completion just before LPIB crosses counts as 1 (early)");

	/* Boundaries at 30 and 40 ms pass; the interrupt is serviced at 45. */
	n = irq_at(45 * 1000000, HDAUDIO_STS_BCIS);
	CHECK(n == 2 && st.st_curblk == 1,
	    "interrupt 15 ms late covers 2 blocks (n=%u)", n);

	/* Boundaries at 50, 60, 70 pass; serviced at 75: a whole ring. */
	n = irq_at(75 * 1000000, HDAUDIO_STS_BCIS);
	CHECK(n == 3 && st.st_curblk == 1 && hdaudio_stats.stream_laps == 1,
	    "interrupt 25 ms late, LPIB back in the same block: a whole lap, "
	    "3 blocks (n=%u)", n);

	n = irq_at(76 * 1000000, HDAUDIO_STS_FIFOE);
	CHECK(n == 0 && hdaudio_stats.stream_fifoe == 1,
	    "FIFO error alone reports no completion");
	n = irq_at(76 * 1000000, HDAUDIO_STS_DESE);
	CHECK(n == 0 && hdaudio_stats.stream_dese == 1,
	    "descriptor error alone reports no completion");
	n = irq_at(76 * 1000000, 0);
	CHECK(n == 0 && hdaudio_stats.stream_spurious == 1,
	    "interrupt without status reports nothing");
	n = irq_at(80 * 1000000 + 10000, HDAUDIO_STS_BCIS | HDAUDIO_STS_FIFOE);
	CHECK(n == 1 && st.st_curblk == 2,
	    "completion with a FIFO error still counts its block");
}

/*
 * The audio layer's view: it moves its ring head one block per callback and
 * refills the block two ahead.  Drive 10000 periods with random interrupt
 * latency (mostly small, sometimes 10-35 ms) and compare the head with the
 * block the DMA engine is in, for the new accounting and for the old rule of
 * one block per interrupt.
 */
static void
test_alignment(void)
{
	uint64_t t = 0, boundary = BLKNS;
	u_int head_new = 0, head_old = 0, bad_new = 0, bad_old = 0, late = 0;

	printf("audio ring alignment, 10000 periods\n");
	stream_start();
	srandom(7);
	for (int p = 0; p < 10000; p++) {
		uint64_t lat = (random() % 100 < 3) ?
		    10000000 + random() % 25000000 : random() % 300000;

		if (lat >= BLKNS)
			late++;
		t = boundary + lat;
		/* Completions latched while waiting merge into this one. */
		while (boundary + BLKNS <= t)
			boundary += BLKNS;
		boundary += BLKNS;

		head_new = (head_new + irq_at(t, HDAUDIO_STS_BCIS)) % 3;
		head_old = (head_old + 1) % 3;
		u_int hw = (lpib_at(t) / BLK) % 3;
		bad_new += head_new != hw;
		bad_old += head_old != hw;
	}
	printf("     %u late interrupts\n", late);
	CHECK(bad_new == 0, "new accounting: head off the DMA block %u times",
	    bad_new);
	CHECK(bad_old > 1000, "old rule (one block per interrupt) for "
	    "comparison: off %u times", bad_old);
}

int
main(int argc, char **argv)
{
	verbose = argc > 1;
	test_transport();
	test_stream();
	test_alignment();
	printf("%d failure(s)\n", failures);
	return failures != 0;
}
