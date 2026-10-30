/*	LispBSD: Intel cAVS 2.5 (Tiger Lake) audio DSP, Sound Open Firmware.	*/

/*
 * The X1 Nano's microphones are two PDM digital mics on the Tiger Lake
 * audio DSP; the HDA codec only sees the headset jack.  This module boots
 * SOF (IPC3, sof-tgl.ri, Intel-signed) on that DSP, beside hdaudio(4),
 * which keeps owning the controller, the codec and its own (coupled)
 * streams.  The sequence follows Linux sound/soc/sof/intel (hda-loader.c,
 * hda-dsp.c, cnl.c, hda-stream.c; dual BSD/GPL), see docs/sof-dsp.org.
 *
 * Phase 1: boot to FW_READY with polled IPC and report the firmware
 * version and mailbox windows; power the DSP down again on unload.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/proc.h>

#include <dev/firmload.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/audio/audio_if.h>
#include <dev/hdaudio/hdaudioreg.h>
#include <dev/hdaudio/hdaudiovar.h>

/* Must match sys/dev/pci/hdaudio_pci.c (we only use the first three). */
struct sofdsp_hdaudio_pci_softc {
	struct hdaudio_softc	sc_hdaudio;
	pcitag_t		sc_tag;
	pci_chipset_tag_t	sc_pc;
};

/* PCI config space (hda.h) */
#define PCI_PGCTL		0x44
#define  PCI_PGCTL_ADSPPGD	__BIT(2)
#define PCI_CGCTL		0x48
#define  PCI_CGCTL_ADSPDCGE	__BIT(1)

/* HDA BAR vendor registers */
#define HDA_VS_INTEL_EM2	0x1030
#define  HDA_VS_INTEL_EM2_L1SEN	__BIT(13)
#define HDA_VS_INTEL_LTRP	0x1048
#define  HDA_VS_INTEL_LTRP_GB_MASK 0x3f
#define HDA_LTRP_GB_VALUE_US	95

/* HDA extended capabilities */
#define HDA_LLCH		0x14
#define HDA_CAP_ID(x)		(((x) >> 16) & 0xfff)
#define HDA_CAP_NEXT(x)		((x) & 0xffff)
#define HDA_PP_CAP_ID		0x3
#define HDA_SPIB_CAP_ID		0x4
#define PP_PPCTL		0x04
#define  PPCTL_GPROCEN		__BIT(30)
#define  PPCTL_PIE		__BIT(31)
#define SPIB_SPBFCCTL		0x04
#define SPIB_SPIB(i)		(0x08 + 0x08 * (i))

/* Stream descriptor bits */
#define SD_CTL_RUN		0x02
#define SD_INT_MASK		0x1c	/* DESE | FIFOE | BCIS */
#define CL_STREAM_FORMAT	0x40

/* DSP BAR (BAR4) */
#define DSP_ADSPCS		0x04
#define  ADSPCS_CRST(m)		((m) << 0)
#define  ADSPCS_CSTALL(m)	((m) << 8)
#define  ADSPCS_SPA(m)		((m) << 16)
#define  ADSPCS_CPA(m)		((m) << 24)
#define DSP_ADSPIC		0x08
#define DSP_ADSPIS		0x0c
#define DSP_HIPCTDR		0xc0
#define DSP_HIPCTDA		0xc4
#define DSP_HIPCTDD		0xc8
#define DSP_HIPCIDR		0xd0
#define DSP_HIPCIDA		0xd4
#define DSP_HIPCIDD		0xd8
#define DSP_HIPCCTL		0xe8
#define  HIPC_BUSY		__BIT(31)	/* TDR, IDR */
#define  HIPC_DONE		__BIT(31)	/* TDA, IDA */
#define  HIPC_MSG_MASK		0x7fffffff

#define SRAM_WINDOW(x)		(0x80000 + (x) * 0x20000)
#define SRAM_ROM_STATUS		(SRAM_WINDOW(0) + 0x0)
#define SRAM_ROM_ERROR		(SRAM_WINDOW(0) + 0x4)
#define MBOX_UPLINK		0x81000
#define  FSR_STATE(x)		((x) & 0xffffff)
#define  FSR_INIT_DONE		0x1
#define  FSR_FW_ENTERED		0x5

#define ROM_IPC_CONTROL		0x01000000
#define ROM_IPC_PURGE_FW	0x00004000

#define CORE0			0x1

/* IPC3 */
#define SOF_GLB_TYPE(x)		((uint32_t)(x) << 28)
#define SOF_GLB_TYPE_MASK	SOF_GLB_TYPE(0xf)
#define SOF_IPC_FW_READY	SOF_GLB_TYPE(0x7)
#define SOF_EXT_MAN_MAGIC	0x6e614d58	/* "XMan" */
#define SOF_IPC_EXT_WINDOW	1	/* enum sof_ipc_ext_data */

struct sof_ipc_hdr {
	uint32_t size;
} __packed;

struct sof_ipc_cmd_hdr {
	uint32_t size;
	uint32_t cmd;
} __packed;

struct sof_ipc_fw_version {
	struct sof_ipc_hdr hdr;
	uint16_t major, minor, micro, build;
	uint8_t date[12];
	uint8_t time[10];
	uint8_t tag[6];
	uint32_t abi_version;
	uint32_t src_hash;
	uint32_t reserved[3];
} __packed;

struct sof_ipc_fw_ready {
	struct sof_ipc_cmd_hdr hdr;
	uint32_t dspbox_offset;
	uint32_t hostbox_offset;
	uint32_t dspbox_size;
	uint32_t hostbox_size;
	struct sof_ipc_fw_version version;
	uint64_t flags;
	uint32_t reserved[4];
} __packed;

struct sof_ipc_ext_data_hdr {
	struct sof_ipc_cmd_hdr hdr;
	uint32_t type;
} __packed;

struct sof_ipc_window_elem {
	struct sof_ipc_hdr hdr;
	uint32_t type;
	uint32_t id;
	uint32_t flags;
	uint32_t size;
	uint32_t offset;
} __packed;

struct sof_ipc_window {
	struct sof_ipc_ext_data_hdr ext_hdr;
	uint32_t num_windows;
	struct sof_ipc_window_elem window[];
} __packed;

struct sof_ext_man_header {
	uint32_t magic;
	uint32_t full_size;
	uint32_t header_size;
	uint32_t header_version;
} __packed;

struct sofdsp_softc {
	struct hdaudio_softc	*sc_hda;
	pci_chipset_tag_t	sc_pc;
	pcitag_t		sc_tag;
	bus_space_tag_t		sc_dspt;
	bus_space_handle_t	sc_dsph;
	bus_size_t		sc_dspsize;
	bool			sc_mapped;
	bool			sc_rtuse;
	bus_size_t		sc_pp, sc_spib;
	u_int			sc_niss;
	bus_dma_tag_t		sc_dmat32;	/* DSP buffers below 4 GB */
	struct hdaudio_dma	sc_bdl[HDAUDIO_MAX_STREAMS];
	uint32_t		sc_hostbox, sc_dspbox;
	uint32_t		sc_hostbox_size, sc_dspbox_size;
};

static struct sofdsp_softc sofdsp_sc;
static const char *sofdsp_fwname = "sof-tgl.ri";

static int dsp_dma_alloc(struct sofdsp_softc *, struct hdaudio_dma *, int);
static void dsp_dma_free(struct sofdsp_softc *, struct hdaudio_dma *);

#define dsp_read4(sc, o)	bus_space_read_4((sc)->sc_dspt, (sc)->sc_dsph, (o))
#define dsp_write4(sc, o, v)	bus_space_write_4((sc)->sc_dspt, (sc)->sc_dsph, (o), (v))

static void
dsp_update4(struct sofdsp_softc *sc, bus_size_t o, uint32_t mask, uint32_t val)
{
	uint32_t v = dsp_read4(sc, o);

	dsp_write4(sc, o, (v & ~mask) | (val & mask));
}

static void
hda_update4(struct sofdsp_softc *sc, bus_size_t o, uint32_t mask, uint32_t val)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	uint32_t v = hda_read4(hda, o);

	hda_write4(hda, o, (v & ~mask) | (val & mask));
}

static void
pci_update4(struct sofdsp_softc *sc, int o, uint32_t mask, uint32_t val)
{
	pcireg_t v = pci_conf_read(sc->sc_pc, sc->sc_tag, o);

	pci_conf_write(sc->sc_pc, sc->sc_tag, o, (v & ~mask) | (val & mask));
}

/* Poll a DSP register until (reg & mask) == want; usec timeout. */
static int
dsp_poll(struct sofdsp_softc *sc, bus_size_t o, uint32_t mask, uint32_t want,
    u_int us, uint32_t *last)
{
	uint32_t v;

	for (;;) {
		v = dsp_read4(sc, o);
		if ((v & mask) == want)
			break;
		if (us < 100) {
			if (last)
				*last = v;
			return ETIMEDOUT;
		}
		DELAY(100);
		us -= 100;
	}
	if (last)
		*last = v;
	return 0;
}

static void
rom_dump(struct sofdsp_softc *sc, const char *why)
{
	int i;

	printf("sofdsp: %s: extended rom status", why);
	for (i = 0; i < 8; i++)
		printf(" %#x", dsp_read4(sc, SRAM_ROM_STATUS + 4 * i));
	printf(", adspcs %#x hipcida %#x hipctdr %#x\n",
	    dsp_read4(sc, DSP_ADSPCS), dsp_read4(sc, DSP_HIPCIDA),
	    dsp_read4(sc, DSP_HIPCTDR));
}

static int
rom_poll(struct sofdsp_softc *sc, uint32_t state, u_int ms)
{
	int deadline = getticks() + MAX(1, mstohz(ms));
	uint32_t v;

	for (;;) {
		v = dsp_read4(sc, SRAM_ROM_STATUS);
		if (FSR_STATE(v) == state)
			return 0;
		if ((v & 0x80000000) || getticks() - deadline > 0)
			break;
		DELAY(50);	/* busy: keep the package out of deep C-states */
	}
	printf("sofdsp: rom status %#x (want %#x)\n", v, state);
	rom_dump(sc, "rom");
	return ETIMEDOUT;
}

/* Overall stream index -> DSP host DMA tag (index within its direction + 1) */
static int
stream_tag(struct sofdsp_softc *sc, struct hdaudio_stream *st)
{
	return st->st_type == HDAUDIO_STREAM_ISS ?
	    st->st_shift + 1 : st->st_shift - (int)sc->sc_niss + 1;
}

static int
stream_nointr(struct hdaudio_stream *st)
{
	struct hdaudio_softc *hda = st->st_host;

	hda_write1(hda, HDAUDIO_SD_STS(st->st_shift), SD_INT_MASK);
	return 1;
}

/*
 * Decoupled stream for the DSP: two BDL entries over a contiguous buffer,
 * the DSP's tag, no controller interrupt.
 */
static void
stream_setup(struct sofdsp_softc *sc, struct hdaudio_stream *st,
    uint16_t fmt, bus_size_t size, bool iccmax)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	struct hdaudio_bdl_entry *bdl = DMA_KERNADDR(&sc->sc_bdl[st->st_shift]);
	uint64_t a = DMA_DMAADDR(&st->st_data);
	int n = st->st_shift;
	bus_size_t chunk, off;
	u_int nbdl;
	uint8_t ctl2;

	if (!iccmax) {
		hda_update4(sc, sc->sc_pp + PP_PPCTL, __BIT(n), __BIT(n));
		hda_write1(hda, HDAUDIO_SD_CTL0(n), hda_read1(hda,
		    HDAUDIO_SD_CTL0(n)) & ~(SD_CTL_RUN | SD_INT_MASK));
		hda_write1(hda, HDAUDIO_SD_STS(n), SD_INT_MASK);
		hdaudio_stream_reset(st);
	}
	hda_write4(hda, HDAUDIO_SD_BDPL(n), 0);
	hda_write4(hda, HDAUDIO_SD_BDPU(n), 0);

	/* 4 KB entries, as Linux's scatter-gather pages give the DSP. */
	chunk = MAX(4096, roundup(howmany(size, HDAUDIO_BDL_MAX), 4096));
	for (nbdl = 0, off = 0; off < size; nbdl++, off += chunk) {
		bdl[nbdl].address_lo = (uint32_t)(a + off);
		bdl[nbdl].address_hi = (a + off) >> 32;
		bdl[nbdl].length = MIN(chunk, size - off);
		bdl[nbdl].flags = (off + chunk >= size) ?
		    HDAUDIO_BDL_ENTRY_IOC : 0;
	}
	if (nbdl == 1) {	/* the controller wants two */
		bdl[0].length = size / 2;
		bdl[1] = bdl[0];
		bdl[0].flags = 0;
		bdl[1].address_lo = (uint32_t)(a + size / 2);
		bdl[1].address_hi = (a + size / 2) >> 32;
		bdl[1].length = size - size / 2;
		nbdl = 2;
	}
	bus_dmamap_sync(sc->sc_dmat32, sc->sc_bdl[n].dma_map, 0,
	    sc->sc_bdl[n].dma_size, BUS_DMASYNC_PREWRITE);

	if (!iccmax) {
		ctl2 = hda_read1(hda, HDAUDIO_SD_CTL2(n)) & ~0xf0;
		hda_write1(hda, HDAUDIO_SD_CTL2(n),
		    ctl2 | (stream_tag(sc, st) << 4));
	}
	hda_write4(hda, HDAUDIO_SD_CBL(n), size);
	if (!iccmax)
		hda_write2(hda, HDAUDIO_SD_FMT(n), fmt);
	hda_write2(hda, HDAUDIO_SD_LVI(n), nbdl - 1);
	a = DMA_DMAADDR(&sc->sc_bdl[n]);
	hda_write4(hda, HDAUDIO_SD_BDPL(n), (uint32_t)a);
	hda_write4(hda, HDAUDIO_SD_BDPU(n), (uint32_t)(a >> 32));
	if (iccmax)
		hda_update4(sc, sc->sc_pp + PP_PPCTL, __BIT(n), __BIT(n));
	else	/* stream interrupt enables, as hda_dsp_stream_hw_params */
		hda_write1(hda, HDAUDIO_SD_CTL0(n),
		    hda_read1(hda, HDAUDIO_SD_CTL0(n)) | SD_INT_MASK);
}

static void
stream_dump(struct sofdsp_softc *sc, struct hdaudio_stream *st, const char *name)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	int n = st->st_shift;

	printf("sofdsp: %s stream %d: ctl %#x sts %#x lpib %#x cbl %#x lvi %u"
	    " fmt %#x bdl %#x:%08x data %#" PRIx64 " spib %#x spbfcctl %#x"
	    " ppctl %#x pphc llp %#x:%08x ldp %#x:%08x\n", name, n,
	    hda_read4(hda, HDAUDIO_SD_CTL0(n)) & 0xffffff,
	    hda_read1(hda, HDAUDIO_SD_STS(n)),
	    hda_read4(hda, HDAUDIO_SD_LPIB(n)),
	    hda_read4(hda, HDAUDIO_SD_CBL(n)),
	    hda_read2(hda, HDAUDIO_SD_LVI(n)),
	    hda_read2(hda, HDAUDIO_SD_FMT(n)),
	    hda_read4(hda, HDAUDIO_SD_BDPU(n)),
	    hda_read4(hda, HDAUDIO_SD_BDPL(n)),
	    (uint64_t)DMA_DMAADDR(&st->st_data),
	    hda_read4(hda, sc->sc_spib + SPIB_SPIB(n)),
	    hda_read4(hda, sc->sc_spib + SPIB_SPBFCCTL),
	    hda_read4(hda, sc->sc_pp + PP_PPCTL),
	    hda_read4(hda, sc->sc_pp + 0x10 + 0x10 * n + 0x4),
	    hda_read4(hda, sc->sc_pp + 0x10 + 0x10 * n + 0x0),
	    hda_read4(hda, sc->sc_pp + 0x10 + 0x10 * n + 0xc),
	    hda_read4(hda, sc->sc_pp + 0x10 + 0x10 * n + 0x8));
}

static void
stream_run(struct sofdsp_softc *sc, struct hdaudio_stream *st, bool run)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	int n = st->st_shift;
	uint8_t c = hda_read1(hda, HDAUDIO_SD_CTL0(n));

	c = run ? (c | SD_CTL_RUN) : (c & ~SD_CTL_RUN);
	hda_write1(hda, HDAUDIO_SD_CTL0(n), c);
}

static void
stream_teardown(struct sofdsp_softc *sc, struct hdaudio_stream *st)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	int n = st->st_shift;

	stream_run(sc, st, false);
	hda_update4(sc, HDAUDIO_MMIO_INTCTL, __BIT(n), 0);
	hda_write4(hda, HDAUDIO_SD_BDPL(n), 0);
	hda_write4(hda, HDAUDIO_SD_BDPU(n), 0);
	hda_write1(hda, HDAUDIO_SD_CTL0(n), 0);
	hda_update4(sc, sc->sc_pp + PP_PPCTL, __BIT(n), 0);
	dsp_dma_free(sc, &st->st_data);
	dsp_dma_free(sc, &sc->sc_bdl[n]);
	hdaudio_stream_disestablish(st);
}

/* hdaudio_dma_alloc with our tag: the DSP gateway may not take 64-bit. */
static int
dsp_dma_alloc(struct sofdsp_softc *sc, struct hdaudio_dma *dma, int flags)
{
	bus_dma_tag_t t = sc->sc_dmat32;
	int err;

	err = bus_dmamem_alloc(t, dma->dma_size, PAGE_SIZE, 0, dma->dma_segs,
	    1, &dma->dma_nsegs, BUS_DMA_WAITOK);
	if (err)
		return err;
	err = bus_dmamem_map(t, dma->dma_segs, dma->dma_nsegs, dma->dma_size,
	    &dma->dma_addr, BUS_DMA_WAITOK | flags);
	if (err)
		goto free;
	err = bus_dmamap_create(t, dma->dma_size, dma->dma_nsegs,
	    dma->dma_size, 0, BUS_DMA_WAITOK, &dma->dma_map);
	if (err)
		goto unmap;
	err = bus_dmamap_load(t, dma->dma_map, dma->dma_addr, dma->dma_size,
	    NULL, BUS_DMA_WAITOK | flags);
	if (err)
		goto destroy;
	memset(dma->dma_addr, 0, dma->dma_size);
	dma->dma_valid = true;
	return 0;
destroy:
	bus_dmamap_destroy(t, dma->dma_map);
unmap:
	bus_dmamem_unmap(t, dma->dma_addr, dma->dma_size);
free:
	bus_dmamem_free(t, dma->dma_segs, dma->dma_nsegs);
	return err;
}

static void
dsp_dma_free(struct sofdsp_softc *sc, struct hdaudio_dma *dma)
{
	bus_dma_tag_t t = sc->sc_dmat32;

	if (!dma->dma_valid)
		return;
	bus_dmamap_unload(t, dma->dma_map);
	bus_dmamap_destroy(t, dma->dma_map);
	bus_dmamem_unmap(t, dma->dma_addr, dma->dma_size);
	bus_dmamem_free(t, dma->dma_segs, dma->dma_nsegs);
	dma->dma_valid = false;
}

static struct hdaudio_stream *
stream_get(struct sofdsp_softc *sc, enum hdaudio_stream_type type,
    bus_size_t size)
{
	struct hdaudio_stream *st;

	st = hdaudio_stream_establish(sc->sc_hda, type, stream_nointr, sc);
	if (st == NULL)
		return NULL;
	st->st_data.dma_size = size;
	st->st_data.dma_sizereg = 0;
	if (dsp_dma_alloc(sc, &st->st_data, BUS_DMA_COHERENT)) {
		hdaudio_stream_disestablish(st);
		return NULL;
	}
	sc->sc_bdl[st->st_shift].dma_size =
	    sizeof(struct hdaudio_bdl_entry) * HDAUDIO_BDL_MAX;
	if (dsp_dma_alloc(sc, &sc->sc_bdl[st->st_shift],
	    BUS_DMA_COHERENT | BUS_DMA_NOCACHE)) {
		dsp_dma_free(sc, &st->st_data);
		hdaudio_stream_disestablish(st);
		return NULL;
	}
	return st;
}

static int
core_power_down(struct sofdsp_softc *sc, uint32_t m)
{
	int err;

	dsp_update4(sc, DSP_ADSPCS, ADSPCS_CSTALL(m), ADSPCS_CSTALL(m));
	dsp_update4(sc, DSP_ADSPCS, ADSPCS_CRST(m), ADSPCS_CRST(m));
	err = dsp_poll(sc, DSP_ADSPCS, ADSPCS_CRST(m), ADSPCS_CRST(m),
	    50000, NULL);
	dsp_update4(sc, DSP_ADSPCS, ADSPCS_SPA(m), 0);
	if (dsp_poll(sc, DSP_ADSPCS, ADSPCS_CPA(m), 0, 50000, NULL))
		err = ETIMEDOUT;
	return err;
}

static void
clock_power_gating(struct sofdsp_softc *sc, bool enable)
{
	pci_update4(sc, PCI_CGCTL, PCI_CGCTL_ADSPDCGE,
	    enable ? PCI_CGCTL_ADSPDCGE : 0);
	hda_update4(sc, HDA_VS_INTEL_EM2, HDA_VS_INTEL_EM2_L1SEN,
	    enable ? HDA_VS_INTEL_EM2_L1SEN : 0);
	pci_update4(sc, PCI_PGCTL, PCI_PGCTL_ADSPPGD,
	    enable ? 0 : PCI_PGCTL_ADSPPGD);
}

/* ROM handshake: power core 0, tell the ROM which stream holds the image. */
static int
cl_dsp_init(struct sofdsp_softc *sc, int tag)
{
	uint32_t v;
	int err;

	dsp_update4(sc, DSP_ADSPCS, ADSPCS_SPA(CORE0), ADSPCS_SPA(CORE0));
	if ((err = dsp_poll(sc, DSP_ADSPCS, ADSPCS_CPA(CORE0),
	    ADSPCS_CPA(CORE0), 50000, &v)) != 0) {
		printf("sofdsp: core 0 power up failed, adspcs %#x\n", v);
		return err;
	}

	dsp_write4(sc, DSP_HIPCIDR, HIPC_BUSY | ROM_IPC_CONTROL |
	    ROM_IPC_PURGE_FW | ((tag - 1) << 9));

	dsp_update4(sc, DSP_ADSPCS, ADSPCS_CRST(CORE0), 0);
	if ((err = dsp_poll(sc, DSP_ADSPCS, ADSPCS_CRST(CORE0), 0, 50000,
	    &v)) != 0) {
		printf("sofdsp: core 0 reset leave failed, adspcs %#x\n", v);
		goto fail;
	}
	dsp_update4(sc, DSP_ADSPCS, ADSPCS_CSTALL(CORE0), 0);
	v = dsp_read4(sc, DSP_ADSPCS);
	printf("sofdsp: core 0 running, adspcs %#x\n", v);

	if ((err = dsp_poll(sc, DSP_HIPCIDA, HIPC_DONE, HIPC_DONE, 500000,
	    &v)) != 0) {
		printf("sofdsp: no ROM ack, hipcida %#x rom %#x\n", v,
		    dsp_read4(sc, SRAM_ROM_STATUS));
		goto fail;
	}
	dsp_update4(sc, DSP_HIPCIDA, HIPC_DONE, HIPC_DONE);

	if ((err = rom_poll(sc, FSR_INIT_DONE, 300)) != 0)
		goto fail;
	return 0;
fail:
	core_power_down(sc, CORE0);
	return err;
}

static void
mbox_read(struct sofdsp_softc *sc, uint32_t off, void *buf, size_t len)
{
	bus_space_read_region_4(sc->sc_dspt, sc->sc_dsph, off, buf, len / 4);
}

static void
ipc_host_done(struct sofdsp_softc *sc)
{
	dsp_update4(sc, DSP_HIPCTDR, HIPC_BUSY, HIPC_BUSY);
	dsp_update4(sc, DSP_HIPCTDA, HIPC_DONE, HIPC_DONE);
}

static int
wait_fw_ready(struct sofdsp_softc *sc)
{
	struct sof_ipc_cmd_hdr hdr;
	struct sof_ipc_fw_ready fr;
	struct sof_ipc_ext_data_hdr eh;
	uint32_t v, off;
	u_int i;
	int ms, start;

	start = getticks();
	for (;;) {
		v = dsp_read4(sc, DSP_HIPCTDR);
		if ((v & HIPC_BUSY) || getticks() - start > mstohz(3000))
			break;
		kpause("sofrdy", false, 1, NULL);
	}
	ms = (getticks() - start) * 1000 / hz;
	if (!(v & HIPC_BUSY)) {
		rom_dump(sc, "no FW_READY");
		return ETIMEDOUT;
	}

	mbox_read(sc, MBOX_UPLINK, &hdr, sizeof(hdr));
	printf("sofdsp: dsp message hipctdr %#x cmd %#x size %u after %d ms\n",
	    v, hdr.cmd, hdr.size, ms);
	if ((hdr.cmd & SOF_GLB_TYPE_MASK) != SOF_IPC_FW_READY) {
		ipc_host_done(sc);
		return EPROTO;
	}

	mbox_read(sc, MBOX_UPLINK, &fr, sizeof(fr));
	printf("sofdsp: SOF %u.%u.%u build %u abi %#x tag %.6s, %.12s %.10s,"
	    " flags %#" PRIx64 "\n",
	    fr.version.major, fr.version.minor, fr.version.micro,
	    fr.version.build, fr.version.abi_version, fr.version.tag,
	    fr.version.date, fr.version.time, fr.flags);

	/* Extended data: the mailbox windows. */
	off = MBOX_UPLINK + sizeof(fr);
	for (;;) {
		mbox_read(sc, off, &eh, sizeof(eh));
		if (eh.hdr.cmd != SOF_IPC_FW_READY || eh.hdr.size < sizeof(eh) ||
		    eh.hdr.size > 4096)
			break;
		if (eh.type == SOF_IPC_EXT_WINDOW) {
			struct sof_ipc_window *w = kmem_alloc(eh.hdr.size,
			    KM_SLEEP);

			mbox_read(sc, off, w, roundup(eh.hdr.size, 4));
			for (i = 0; i < w->num_windows &&
			    sizeof(*w) + (i + 1) * sizeof(w->window[0]) <=
			    eh.hdr.size; i++) {
				struct sof_ipc_window_elem *e = &w->window[i];
				uint32_t base = SRAM_WINDOW(e->id) + e->offset;

				printf("sofdsp: window type %u id %u off %#x"
				    " size %#x -> %#x\n", e->type, e->id,
				    e->offset, e->size, base);
				if (e->type == 0) {	/* DOWNBOX: host -> dsp */
					sc->sc_hostbox = base;
					sc->sc_hostbox_size = e->size;
				} else if (e->type == 1) {	/* UPBOX */
					sc->sc_dspbox = base;
					sc->sc_dspbox_size = e->size;
				}
			}
			kmem_free(w, eh.hdr.size);
		} else
			printf("sofdsp: ext data type %u size %u\n", eh.type,
			    eh.hdr.size);
		off += eh.hdr.size;
	}

	ipc_host_done(sc);
	return 0;
}

static int
sofdsp_boot(struct sofdsp_softc *sc)
{
	struct hdaudio_softc *hda = sc->sc_hda;
	struct hdaudio_stream *cl = NULL, *icc = NULL;
	firmware_handle_t fh;
	const struct sof_ext_man_header *em;
	uint8_t *fw = NULL;
	size_t fwsize = 0, off;
	uint8_t ltrp;
	int err, tag, i;

	err = firmware_open("sofdsp", sofdsp_fwname, &fh);
	if (err) {
		printf("sofdsp: firmware %s: error %d\n", sofdsp_fwname, err);
		return err;
	}
	fwsize = firmware_get_size(fh);
	fw = firmware_malloc(fwsize);
	err = firmware_read(fh, 0, fw, fwsize);
	firmware_close(fh);
	if (err)
		goto out;
	em = (const void *)fw;
	off = (fwsize > sizeof(*em) && em->magic == SOF_EXT_MAN_MAGIC) ?
	    em->full_size : 0;
	if (off >= fwsize) {
		err = EINVAL;
		goto out;
	}
	printf("sofdsp: firmware %zu bytes, image %zu at %#zx\n", fwsize,
	    fwsize - off, off);

	clock_power_gating(sc, false);
	hda_update4(sc, sc->sc_pp + PP_PPCTL, PPCTL_GPROCEN, PPCTL_GPROCEN);

	/* ICCMAX capture stream (TGL boots with one running, as Linux does) */
	icc = stream_get(sc, HDAUDIO_STREAM_ISS, PAGE_SIZE);
	if (icc == NULL) {
		err = EBUSY;
		goto out;
	}
	stream_setup(sc, icc, CL_STREAM_FORMAT, PAGE_SIZE, true);
	ltrp = hda_read1(hda, HDA_VS_INTEL_LTRP);
	hda_write1(hda, HDA_VS_INTEL_LTRP,
	    (ltrp & ~HDA_VS_INTEL_LTRP_GB_MASK) | HDA_LTRP_GB_VALUE_US);
	stream_run(sc, icc, true);

	/* Code loader: the image on an output stream, SPIB at its end. */
	cl = stream_get(sc, HDAUDIO_STREAM_OSS, roundup(fwsize - off, 128));
	if (cl == NULL) {
		err = EBUSY;
		goto out;
	}
	memcpy(DMA_KERNADDR(&cl->st_data), fw + off, fwsize - off);
	bus_dmamap_sync(sc->sc_dmat32, cl->st_data.dma_map, 0,
	    cl->st_data.dma_size, BUS_DMASYNC_PREWRITE);
	stream_setup(sc, cl, CL_STREAM_FORMAT, cl->st_data.dma_size, false);
	hda_update4(sc, sc->sc_spib + SPIB_SPBFCCTL, __BIT(cl->st_shift),
	    __BIT(cl->st_shift));
	hda_write4(hda, sc->sc_spib + SPIB_SPIB(cl->st_shift), fwsize - off);
	tag = stream_tag(sc, cl);
	printf("sofdsp: icc stream %d (tag %d), loader stream %d (tag %d)\n",
	    icc->st_shift, stream_tag(sc, icc), cl->st_shift, tag);

	for (i = 0; i < 3; i++) {
		if ((err = cl_dsp_init(sc, tag)) == 0)
			break;
		printf("sofdsp: ROM init attempt %d failed (%d)\n", i + 1, err);
	}
	if (err)
		goto out;

	hda_update4(sc, HDAUDIO_MMIO_INTCTL, __BIT(cl->st_shift),
	    __BIT(cl->st_shift));
	stream_run(sc, cl, true);
	err = rom_poll(sc, FSR_FW_ENTERED, 3000);
	if (err) {
		stream_dump(sc, cl, "loader");
		stream_dump(sc, icc, "iccmax");
	}
	stream_run(sc, cl, false);
	if (err)
		goto out;
	printf("sofdsp: firmware entered\n");

	err = wait_fw_ready(sc);
out:
	if (cl != NULL) {
		hda_update4(sc, sc->sc_spib + SPIB_SPBFCCTL,
		    __BIT(cl->st_shift), 0);
		stream_teardown(sc, cl);
	}
	if (icc != NULL) {
		stream_teardown(sc, icc);
		hda_write1(hda, HDA_VS_INTEL_LTRP, ltrp);
	}
	clock_power_gating(sc, true);
	if (err) {
		core_power_down(sc, CORE0);
		hda_update4(sc, sc->sc_pp + PP_PPCTL, PPCTL_GPROCEN, 0);
	}
	if (fw != NULL)
		firmware_free(fw, fwsize);
	return err;
}

static int
sofdsp_attach(struct sofdsp_softc *sc)
{
	device_t dv;
	struct sofdsp_hdaudio_pci_softc *psc;
	struct hdaudio_softc *hda;
	bus_addr_t base;
	pcireg_t type;
	int flags, err;
	uint32_t cap, off;

	dv = device_find_by_xname("hdaudio0");
	if (dv == NULL)
		return ENXIO;
	psc = device_private(dv);
	hda = &psc->sc_hdaudio;
	sc->sc_hda = hda;
	sc->sc_pc = psc->sc_pc;
	sc->sc_tag = psc->sc_tag;

	if ((err = bus_dmatag_subregion(hda->sc_dmat, 0, 0xffffffffUL,
	    &sc->sc_dmat32, BUS_DMA_WAITOK)) != 0)
		return err;
	if ((err = hdaudio_rt_use(hda)) != 0)
		return err;
	sc->sc_rtuse = true;

	sc->sc_niss = HDAUDIO_GCAP_ISS(hda_read2(hda, HDAUDIO_MMIO_GCAP));
	for (off = hda_read2(hda, HDA_LLCH); off != 0 && off < 0x4000;
	    off = HDA_CAP_NEXT(cap)) {
		cap = hda_read4(hda, off);
		if (HDA_CAP_ID(cap) == HDA_PP_CAP_ID)
			sc->sc_pp = off;
		else if (HDA_CAP_ID(cap) == HDA_SPIB_CAP_ID)
			sc->sc_spib = off;
	}
	printf("sofdsp: hdaudio0 iss %u, pp %#lx, spib %#lx\n", sc->sc_niss,
	    (u_long)sc->sc_pp, (u_long)sc->sc_spib);
	if (sc->sc_pp == 0 || sc->sc_spib == 0)
		return ENXIO;

	type = pci_mapreg_type(sc->sc_pc, sc->sc_tag, PCI_BAR(4));
	if (pci_mapreg_info(sc->sc_pc, sc->sc_tag, PCI_BAR(4), type, &base,
	    &sc->sc_dspsize, &flags))
		return ENXIO;
	sc->sc_dspt = hda->sc_memt;
	if ((err = bus_space_map(sc->sc_dspt, base, sc->sc_dspsize, 0,
	    &sc->sc_dsph)) != 0)
		return err;
	sc->sc_mapped = true;
	printf("sofdsp: dsp bar %#" PRIxBUSADDR " size %#" PRIxBUSSIZE
	    ", adspcs %#x, rom status %#x\n", base, sc->sc_dspsize,
	    dsp_read4(sc, DSP_ADSPCS), dsp_read4(sc, SRAM_ROM_STATUS));

	return sofdsp_boot(sc);
}

static void
sofdsp_detach(struct sofdsp_softc *sc)
{
	if (sc->sc_mapped) {
		core_power_down(sc, CORE0);
		hda_update4(sc, sc->sc_pp + PP_PPCTL, PPCTL_GPROCEN, 0);
		printf("sofdsp: dsp off, adspcs %#x\n",
		    dsp_read4(sc, DSP_ADSPCS));
		bus_space_unmap(sc->sc_dspt, sc->sc_dsph, sc->sc_dspsize);
		sc->sc_mapped = false;
	}
	if (sc->sc_rtuse) {
		hdaudio_rt_release(sc->sc_hda);
		sc->sc_rtuse = false;
	}
}

MODULE(MODULE_CLASS_MISC, sofdsp, NULL);

static int
sofdsp_modcmd(modcmd_t cmd, void *arg)
{
	int err;

	switch (cmd) {
	case MODULE_CMD_INIT:
		memset(&sofdsp_sc, 0, sizeof(sofdsp_sc));
		if (arg != NULL)
			prop_dictionary_get_cstring_nocopy(arg, "fw",
			    &sofdsp_fwname);
		err = sofdsp_attach(&sofdsp_sc);
		if (err)
			sofdsp_detach(&sofdsp_sc);
		return err;
	case MODULE_CMD_FINI:
		sofdsp_detach(&sofdsp_sc);
		return 0;
	default:
		return ENOTTY;
	}
}
