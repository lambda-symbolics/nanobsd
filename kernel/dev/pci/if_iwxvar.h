/*	$NetBSD$	*/
/*	OpenBSD: if_iwxvar.h,v 1.50 2026/03/20 08:38:21 stsp Exp	*/

/*
 * Copyright (c) 2014 genua mbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Based on BSD-licensed source modules in the Linux iwlwifi driver,
 * which were used as the reference documentation for this implementation.
 *
 ******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************
 */

/*-
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

struct iwx_rx_radiotap_header {
	struct ieee80211_radiotap_header wr_ihdr;
	uint64_t	wr_tsft;
	uint8_t		wr_flags;
	uint8_t		wr_rate;
	uint16_t	wr_chan_freq;
	uint16_t	wr_chan_flags;
	int8_t		wr_dbm_antsignal;
	int8_t		wr_dbm_antnoise;
};

#define IWX_RX_RADIOTAP_PRESENT						\
	((1 << IEEE80211_RADIOTAP_TSFT) |				\
	 (1 << IEEE80211_RADIOTAP_FLAGS) |				\
	 (1 << IEEE80211_RADIOTAP_RATE) |				\
	 (1 << IEEE80211_RADIOTAP_CHANNEL) |				\
	 (1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |			\
	 (1 << IEEE80211_RADIOTAP_DBM_ANTNOISE))

struct iwx_tx_radiotap_header {
	struct ieee80211_radiotap_header wt_ihdr;
	uint8_t		wt_flags;
	uint8_t		wt_rate;
	uint16_t	wt_chan_freq;
	uint16_t	wt_chan_flags;
};

#define IWX_TX_RADIOTAP_PRESENT						\
	((1 << IEEE80211_RADIOTAP_FLAGS) |				\
	 (1 << IEEE80211_RADIOTAP_RATE) |				\
	 (1 << IEEE80211_RADIOTAP_CHANNEL))

#define IWX_UCODE_SECT_MAX 69

/*
 * fw_status is used to determine if we've already parsed the firmware file
 *
 * In addition to the following, status < 0 ==> -error
 */
#define IWX_FW_STATUS_NONE		0
#define IWX_FW_STATUS_INPROGRESS	1
#define IWX_FW_STATUS_DONE		2

enum iwx_ucode_type {
	IWX_UCODE_TYPE_REGULAR,
	IWX_UCODE_TYPE_INIT,
	IWX_UCODE_TYPE_WOW,
	IWX_UCODE_TYPE_REGULAR_USNIFFER,
	IWX_UCODE_TYPE_MAX
};

struct iwx_fw_info {
	void *fw_rawdata;
	size_t fw_rawsize;
	int fw_status;

	struct iwx_fw_sects {
		struct iwx_fw_onesect {
			void *fws_data;
			uint32_t fws_len;
			uint32_t fws_devoff;
		} fw_sect[IWX_UCODE_SECT_MAX];
		size_t fw_totlen;
		int fw_count;
	} fw_sects[IWX_UCODE_TYPE_MAX];

	/* FW debug data parsed for driver usage */
	int dbg_dest_tlv_init;
	uint8_t *dbg_dest_ver;
	uint8_t n_dest_reg;
	struct iwx_fw_dbg_dest_tlv_v1 *dbg_dest_tlv_v1;

	struct iwx_fw_dbg_conf_tlv *dbg_conf_tlv[IWX_FW_DBG_CONF_MAX];
	size_t dbg_conf_tlv_len[IWX_FW_DBG_CONF_MAX];
	struct iwx_fw_dbg_trigger_tlv *dbg_trigger_tlv[IWX_FW_DBG_TRIGGER_MAX];
	size_t dbg_trigger_tlv_len[IWX_FW_DBG_TRIGGER_MAX];
	struct iwx_fw_dbg_mem_seg_tlv *dbg_mem_tlv;
	size_t n_mem_tlv;

	/* Copy of firmware image loader found in file. */
	uint8_t *iml;
	size_t iml_len;

	/*
	 * Copy of PNVM image found in file.
	 * Used in preference to external .pnvm file if present.
	 */
	uint8_t *pnvm;
	size_t pnvm_len;
};

struct iwx_nvm_data {
	int n_hw_addrs;
	uint8_t hw_addr[ETHER_ADDR_LEN];

	int sku_cap_band_24GHz_enable;
	int sku_cap_band_52GHz_enable;
	int sku_cap_11n_enable;
	int sku_cap_11ac_enable;
	int sku_cap_11ax_enable;
	int sku_cap_amt_enable;
	int sku_cap_ipan_enable;
	int sku_cap_mimo_disable;
	int lar_enabled;

	uint8_t valid_tx_ant, valid_rx_ant;

	uint16_t nvm_version;
};

/* max bufs per tfd the driver will use */
#define IWX_MAX_CMD_TBS_PER_TFD 2

struct iwx_host_cmd {
	const void *data[IWX_MAX_CMD_TBS_PER_TFD];
	struct iwx_rx_packet *resp_pkt;
	size_t resp_pkt_len;
	unsigned long _rx_page_addr;
	uint32_t _rx_page_order;
	int handler_status;

	uint32_t flags;
	uint16_t len[IWX_MAX_CMD_TBS_PER_TFD];
	uint8_t dataflags[IWX_MAX_CMD_TBS_PER_TFD];
	uint32_t id;
};

/*
 * DMA glue is from iwn
 */

struct iwx_dma_info {
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	bus_dma_segment_t	seg;
	bus_addr_t		paddr;
	void 			*vaddr;
	bus_size_t		size;
};

#define IWX_TX_RING_COUNT	IWX_DEFAULT_QUEUE_SIZE
#define IWX_TX_RING_LOMARK	192
#define IWX_TX_RING_HIMARK	224

struct iwx_tx_data {
	bus_dmamap_t	map;
	bus_addr_t	cmd_paddr;
	struct mbuf	*m;
	struct iwx_node *in;
	int flags;
#define IWX_TXDATA_FLAG_CMD_IS_NARROW	0x01
};

struct iwx_tx_ring {
	struct iwx_dma_info	desc_dma;
	struct iwx_dma_info	cmd_dma;
	struct iwx_dma_info	bc_tbl;
	struct iwx_tfh_tfd	*desc;
	struct iwx_device_cmd	*cmd;
	struct iwx_tx_data	data[IWX_TX_RING_COUNT];
	int			qid;
	int			queued;
	int			cur;
	int			cur_hw;
	int			tail;
	int			tail_hw;
	int			tid;
};

#define IWX_RX_MQ_RING_COUNT	512
/* Linux driver optionally uses 8k buffer */
#define IWX_RBUF_SIZE		4096

struct iwx_rx_data {
	struct mbuf	*m;
	bus_dmamap_t	map;
};

struct iwx_rx_ring {
	struct iwx_dma_info	free_desc_dma;
	struct iwx_dma_info	stat_dma;
	struct iwx_dma_info	used_desc_dma;
	void			*desc;
	struct iwx_rb_status	*stat;
	struct iwx_rx_data	data[IWX_RX_MQ_RING_COUNT];
	int			cur;
};

#define IWX_FLAG_USE_ICT	0x01	/* using Interrupt Cause Table */
#define IWX_FLAG_RFKILL		0x02	/* radio kill switch is set */
#define IWX_FLAG_SCANNING	0x04	/* scan in progress */
#define IWX_FLAG_MAC_ACTIVE	0x08	/* MAC context added to firmware */
#define IWX_FLAG_BINDING_ACTIVE	0x10	/* MAC->PHY binding added to firmware */
#define IWX_FLAG_STA_ACTIVE	0x20	/* AP added to firmware station table */
#define IWX_FLAG_TE_ACTIVE	0x40	/* time event is scheduled */
#define IWX_FLAG_HW_ERR		0x80	/* hardware error occurred */
#define IWX_FLAG_SHUTDOWN	0x100	/* shutting down; new tasks forbidden */
#define IWX_FLAG_BGSCAN		0x200	/* background scan in progress */
#define IWX_FLAG_TXFLUSH	0x400	/* Tx queue flushing in progress */
#define IWX_FLAG_PHY_ACTIVE	0x800	/* PHY context added to firmware */
#define IWX_FLAG_ATTACHED	0x1000	/* ifnet/net80211 attached */

struct iwx_ucode_status {
	uint32_t uc_lmac_error_event_table[2];
	uint32_t uc_umac_error_event_table;
	uint32_t uc_log_event_table;
	unsigned int error_event_table_tlv_status;

	int uc_ok;
	int uc_intr;
};

#define IWX_ERROR_EVENT_TABLE_LMAC1	(1 << 0)
#define IWX_ERROR_EVENT_TABLE_LMAC2	(1 << 1)
#define IWX_ERROR_EVENT_TABLE_UMAC	(1 << 2)

#define IWX_CMD_RESP_MAX PAGE_SIZE

#define IWX_TE_SESSION_PROTECTION_MAX_TIME_MS 1000
#define IWX_TE_SESSION_PROTECTION_MIN_TIME_MS 400

enum IWX_CMD_MODE {
	IWX_CMD_ASYNC		= (1 << 0),
	IWX_CMD_WANT_RESP	= (1 << 1),
	IWX_CMD_SEND_IN_RFKILL	= (1 << 2),
};
enum iwx_hcmd_dataflag {
	IWX_HCMD_DFL_NOCOPY     = (1 << 0),
	IWX_HCMD_DFL_DUP        = (1 << 1),
};

#define IWX_NUM_PAPD_CH_GROUPS	9
#define IWX_NUM_TXP_CH_GROUPS	9

struct iwx_phy_ctxt {
	uint16_t id;
	uint16_t color;
	uint32_t ref;
	struct ieee80211_channel *channel;
	uint8_t sco; /* 40 MHz secondary channel offset */
	uint8_t vht_chan_width;
};

/*
 * LISPBSD: 802.11n receive Block Ack, as in OpenBSD.  The firmware
 * de-aggregates A-MPDUs and tags each MPDU with its BAID, sequence
 * number and the window's next expected one; the driver reorders.
 *
 * struct iwx_reorder_buffer - per BAID reorder window
 * @head_sn: reorder window head sn
 * @num_stored: number of MPDUs stored in the buffer
 * @buf_size: the reorder buffer size as set by the last ADDBA request
 * @valid: reordering is valid (the first in-window frame was seen)
 */
struct iwx_reorder_buffer {
	uint16_t head_sn;
	uint16_t num_stored;
	uint16_t buf_size;
	int valid;
};

/* Frames stored for one sequence number (several for an A-MSDU). */
struct iwx_reorder_buf_entry {
	struct mbuf *frames;		/* linked by m_nextpkt */
	struct mbuf **frames_tail;
	uint32_t rx_pkt_status;
	int chanidx;
	int is_shortpre;
	uint32_t rate_n_flags;
	uint32_t device_timestamp;
	int rssi;
};

struct iwx_rxba_data {
	uint8_t sta_id;
	uint8_t tid;
	uint8_t baid;
	uint32_t timeout;		/* session timeout, usec (0: none) */
	struct timeval last_rx;
	callout_t session_timer;
	callout_t reorder_timer;	/* releases frames stuck behind a gap */
	struct iwx_softc *sc;
	struct iwx_reorder_buffer reorder_buf;
	struct iwx_reorder_buf_entry entries[IEEE80211_BA_MAX_WINSZ];
};

struct iwx_ba_task_data {
	uint32_t start_tidmask;
	uint32_t stop_tidmask;
};

/*
 * LISPBSD: U-APSD state for hw.iwx.uapsd_status (what the AP offered,
 * what was negotiated and the power table last sent to the firmware).
 */
struct iwx_uapsd_diag {
	int		ud_assoc;	/* associated (iwx_run done) */
	int		ud_ap_wmm;	/* AP's beacon carried a WMM element */
	int		ud_ap_uapsd;	/* ...advertising U-APSD */
	uint8_t		ud_req;		/* QoS Info of our assoc request */
	int		ud_qos;		/* WMM (QoS) association */
	int		ud_negotiated;	/* IEEE80211_NODE_UAPSD at iwx_run */
	int		ud_active;	/* last MAC power table had U-APSD */
	int		ud_pslevel;	/* power level of that table */
	uint32_t	ud_assocs;	/* associations seen */
	uint32_t	ud_uapsd_assocs; /* ...of which with U-APSD */
	uint32_t	ud_misbehaving;	/* misbehaving-AP notifications */
	int		ud_off_why;	/* uapsd_task: IWX_UAPSD_OFF_* */
	uint32_t	ud_off_gen;	/* uapsd_task: ud_assocs when queued */
	uint8_t		ud_sta_acs;	/* ADD_STA/STA_CONFIG uapsd_acs */
	uint8_t		ud_sp_length;	/* ADD_STA/STA_CONFIG sp_length */
	struct iwx_mac_power_cmd ud_pm;	/* last MAC power table sent */
};

struct iwx_bf_data {
	int bf_enabled;		/* filtering	*/
	int ba_enabled;		/* abort	*/
	int ave_beacon_signal;
	int last_cqm_event;
};

/**
 * struct iwx_self_init_dram - dram data used by self init process
 * @fw: lmac and umac dram data
 * @lmac_cnt: number of lmac sections in fw image
 * @umac_cnt: number of umac sections in fw image
 * @paging: paging dram data
 * @paging_cnt: number of paging sections needed by fw image
 */
struct iwx_self_init_dram {
	struct iwx_dma_info *fw;
	int lmac_cnt;
	int umac_cnt;
	struct iwx_dma_info *paging;
	int paging_cnt;
};

/*
 * Device configuration parameters which cannot be detected based on
 * PCI vendor/product ID alone.
 */
struct iwx_device_cfg {
	const char *fw_name;
	const char *pnvm_name;
	int 	    tx_with_siso_diversity;
	int 	    uhb_supported;
	int 	    xtal_latency;
	int 	    low_latency_xtal;
};

/*
 * Firmware images live in /libdata/firmware/if_iwx/ and use the
 * same file names as OpenBSD's fw_update(8) package.
 */
#define IWX_CC_A_FW	    	"iwx-cc-a0-77"
#define IWX_TY_A_GF_A_FW	"iwx-ty-a0-gf-a0-77"
#define IWX_TY_A_GF_A_PNVM	"iwx-ty-a0-gf-a0.pnvm"
#define IWX_QU_B_HR_B_FW	"iwx-Qu-b0-hr-b0-77"
#define IWX_QU_B_JF_B_FW	"iwx-Qu-b0-jf-b0-77"
#define IWX_QU_C_HR_B_FW	"iwx-Qu-c0-hr-b0-77"
#define IWX_QU_C_JF_B_FW	"iwx-Qu-c0-jf-b0-77"
#define IWX_QUZ_A_HR_B_FW	"iwx-QuZ-a0-hr-b0-77"
#define IWX_QUZ_A_JF_B_FW	"iwx-QuZ-a0-jf-b0-77"
#define IWX_SO_A_GF_A_FW	"iwx-so-a0-gf-a0-77"
#define IWX_SO_A_GF_A_PNVM	"iwx-so-a0-gf-a0.pnvm"
#define IWX_SO_A_GF4_A_FW	"iwx-so-a0-gf4-a0-77"
#define IWX_SO_A_GF4_A_PNVM	"iwx-so-a0-gf4-a0.pnvm"
#define IWX_SO_A_HR_B_FW	"iwx-so-a0-hr-b0-77"
#define IWX_SO_A_JF_B_FW	"iwx-so-a0-jf-b0-77"
#define IWX_MA_B_HR_B_FW	"iwx-ma-b0-hr-b0-89"
#define IWX_MA_B_GF_A_FW	"iwx-ma-b0-gf-a0-89"
#define IWX_MA_B_GF_A_PNVM	"iwx-ma-b0-gf-a0.pnvm"
#define IWX_MA_B_GF4_A_FW	"iwx-ma-b0-gf4-a0-89"
#define IWX_MA_B_GF4_A_PNVM	"iwx-ma-b0-gf4-a0.pnvm"
#define IWX_MA_A_FM_A_FW	"iwx-ma-a0-fm-a0-89"
#define IWX_MA_A_FM_A_PNVM	"iwx-ma-a0-fm-a0.pnvm"
#define IWX_BZ_B_GF_A_FW	"iwx-bz-b0-gf-a0-100"
#define IWX_BZ_B_GF_A_PNVM	"iwx-bz-b0-gf-a0.pnvm"

#define IWX_CFG_ANY (~0)

#define IWX_CFG_MAC_TYPE_QU		0x33
#define IWX_CFG_MAC_TYPE_QUZ		0x35
#define IWX_CFG_MAC_TYPE_QNJ		0x36
#define IWX_CFG_MAC_TYPE_SO		0x37
#define IWX_CFG_MAC_TYPE_SNJ		0x42
#define IWX_CFG_MAC_TYPE_SOF		0x43
#define IWX_CFG_MAC_TYPE_MA		0x44
#define IWX_CFG_MAC_TYPE_BZ		0x46
#define IWX_CFG_MAC_TYPE_GL		0x47
#define IWX_CFG_MAC_TYPE_BZ_W		0x4B

#define IWX_CFG_RF_TYPE_JF2		0x105
#define IWX_CFG_RF_TYPE_JF1		0x108
#define IWX_CFG_RF_TYPE_HR2		0x10a
#define IWX_CFG_RF_TYPE_HR1		0x10c
#define IWX_CFG_RF_TYPE_GF		0x10d
#define IWX_CFG_RF_TYPE_MR		0x110
#define IWX_CFG_RF_TYPE_MS		0x111
#define IWX_CFG_RF_TYPE_FM		0x112

#define IWX_CFG_RF_ID_JF		0x3
#define IWX_CFG_RF_ID_JF1		0x6
#define IWX_CFG_RF_ID_JF1_DIV		0xa

#define IWX_CFG_NO_160			0x1
#define IWX_CFG_160			0x0

#define IWX_CFG_CORES_BT		0x0

#define IWX_CFG_NO_CDB			0x0
#define IWX_CFG_CDB			0x1

#define IWX_SUBDEVICE_RF_ID(subdevice)	((uint16_t)((subdevice) & 0x00f0) >> 4)
#define IWX_SUBDEVICE_NO_160(subdevice)	((uint16_t)((subdevice) & 0x0200) >> 9)
#define IWX_SUBDEVICE_CORES(subdevice)	((uint16_t)((subdevice) & 0x1c00) >> 10)

/*
 * Minimal task queue: a kernel thread executing queued tasks in order.
 * This mirrors the OpenBSD task(9) semantics the driver relies on
 * (task_add/task_del/task_pending).
 */
struct iwx_task {
	TAILQ_ENTRY(iwx_task)	t_entry;
	void			(*t_func)(void *);
	void			*t_arg;
	int			t_onqueue;
};

struct iwx_taskq {
	kmutex_t		tq_mtx;
	kcondvar_t		tq_cv;
	TAILQ_HEAD(, iwx_task)	tq_list;
	lwp_t			*tq_lwp;
	int			tq_dying;
	char			tq_name[16];
};

struct iwx_softc {
	device_t sc_dev;
	struct ethercom sc_ec;
	struct ieee80211com sc_ic;
	int (*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	int sc_newstate_pending;
	int attached;

	struct iwx_taskq	*sc_systq;	/* generic task queue */
	struct iwx_taskq	*sc_nswq;	/* newstate task queue */

	struct iwx_task		init_task; /* NB: not reference-counted */
	int			task_refs;
	struct iwx_task		newstate_task;
	enum ieee80211_state	ns_nstate;
	int			ns_arg;
	struct iwx_task		uapsd_task;	/* LISPBSD: U-APSD fallback */
	struct iwx_task		ba_task;	/* LISPBSD: Rx Block Ack setup */
	struct iwx_uapsd_diag	sc_uapsd;	/* LISPBSD: U-APSD state */

	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
	bus_size_t sc_sz;
	bus_dma_tag_t sc_dmat;
	pci_product_id_t sc_pid;
	pci_chipset_tag_t sc_pct;
	pcitag_t sc_pcitag;
	pci_intr_handle_t *sc_pihp;
	void *sc_ih;
	void *sc_soft_ih;
	int sc_msix;

	/* TX/RX rings. */
	struct iwx_tx_ring txq[IWX_NUM_TX_QUEUES];
	struct iwx_rx_ring rxq;
	int qfullmsk;
	int qenablemsk;
	int first_data_qid;
	int aggqid[IWX_MAX_TID_COUNT];
	int max_tfd_queue_size;

	int sc_sf_state;

	/* ICT table. */
	struct iwx_dma_info	ict_dma;
	int			ict_cur;

	int sc_hw_rev;
#define IWX_SILICON_A_STEP	0
#define IWX_SILICON_B_STEP	1
#define IWX_SILICON_C_STEP	2
#define IWX_SILICON_Z_STEP	0xf
	int sc_hw_id;
	int sc_hw_crf_id;
	int sc_hw_cnv_id;
	int sc_hw_rf_id;
	int sc_device_family;
#define IWX_DEVICE_FAMILY_22000	1
#define IWX_DEVICE_FAMILY_AX210	2
#define IWX_DEVICE_FAMILY_BZ	3
	uint32_t sc_sku_id[3];
	uint32_t mac_addr_from_csr;

	struct iwx_dma_info ctxt_info_dma;
	struct iwx_self_init_dram init_dram;
	struct iwx_dma_info prph_scratch_dma;
	struct iwx_dma_info prph_info_dma;
	struct iwx_dma_info iml_dma;
	struct iwx_dma_info pnvm_dma;
	struct iwx_dma_info pnvm_seg_dma[IWX_MAX_DRAM_ENTRY];
	uint32_t pnvm_size;
	int pnvm_segs;
	uint32_t sc_pnvm_ver;

	int sc_fw_chunk_done;
	int sc_init_complete;
#define IWX_INIT_COMPLETE	0x01
#define IWX_CALIB_COMPLETE	0x02
#define IWX_PNVM_COMPLETE	0x04

	int sc_system_stats_cleared;

	struct iwx_ucode_status sc_uc;
	char sc_fwver[32];

	int sc_capaflags;
	int sc_capa_max_probe_len;
	int sc_capa_n_scan_channels;
	uint8_t sc_ucode_api[howmany(IWX_NUM_UCODE_TLV_API, NBBY)];
	uint8_t sc_enabled_capa[howmany(IWX_NUM_UCODE_TLV_CAPA, NBBY)];
#define IWX_MAX_FW_CMD_VERSIONS	704
	struct iwx_fw_cmd_version cmd_versions[IWX_MAX_FW_CMD_VERSIONS];
	int n_cmd_versions;
	int sc_rate_n_flags_version;
	int sc_use_mld_api;

	int sc_intmask;
	int sc_flags;

	uint32_t sc_fh_init_mask;
	uint32_t sc_hw_init_mask;
	uint32_t sc_fh_mask;
	uint32_t sc_hw_mask;

	int sc_generation;

	krwlock_t ioctl_rwl;

	int sc_cap_off; /* PCIe caps */

	const char *sc_fwname;
	struct iwx_fw_info sc_fw;
	struct iwx_dma_info fw_mon;
	int sc_fw_phy_config;
	struct iwx_tlv_calib_ctrl sc_default_calib[IWX_UCODE_TYPE_MAX];

	struct iwx_nvm_data sc_nvm;
	struct iwx_bf_data sc_bf;
	const char *sc_pnvm_name;

	int sc_tx_timer[IWX_NUM_TX_QUEUES];
	int sc_rx_ba_sessions;
	/* LISPBSD: 802.11n */
	struct iwx_rxba_data sc_rxba_data[IWX_MAX_BAID];
	struct iwx_ba_task_data ba_rx;
	struct iwx_ba_task_data ba_tx;	/* start: enable an aggregation queue */
	uint32_t ba_txreq_tidmask;	/* send an ADDBA request */
	struct iwx_ht_stats {		/* hw.iwx.ht_status */
		uint32_t	hs_assoc_ht;	/* HT associations */
		uint32_t	hs_addba;	/* sessions started */
		uint32_t	hs_addba_refused;
		uint32_t	hs_delba;	/* sessions stopped */
		uint32_t	hs_stored;	/* frames held for reordering */
		uint32_t	hs_dropped;	/* duplicate or behind window */
		uint32_t	hs_bar;		/* BAR/firmware releases */
		uint32_t	hs_timer;	/* reorder timer releases */
		uint32_t	hs_rx_rate;	/* last unicast data rx rate_n_flags */
		uint32_t	hs_tx_rate;	/* last rate from firmware TLC */
		uint32_t	hs_assoc_vht;	/* VHT associations */
		uint32_t	hs_rx_vht80;	/* unicast data: VHT at 80 MHz */
		uint32_t	hs_rx_vht;	/* VHT at 20/40 MHz */
		uint32_t	hs_rx_ht40;	/* HT at 40 MHz */
		uint32_t	hs_rx_ht20;
		uint32_t	hs_rx_legacy;
		uint32_t	hs_tx_agg;	/* Tx agreements started */
		uint32_t	hs_tx_agg_frames;	/* frames on agg queues */
		uint32_t	hs_ba_notif;	/* compressed Block Acks */
	} sc_ht_stats;
	uint8_t sc_chan40[howmany(IEEE80211_CHAN_MAX + 1, NBBY)];
	uint8_t sc_chan80[howmany(IEEE80211_CHAN_MAX + 1, NBBY)];

	int sc_scan_last_antenna;

	int sc_staid;
	int sc_nodecolor;

	uint8_t *sc_cmd_resp_pkt[IWX_TX_RING_COUNT];
	size_t sc_cmd_resp_len[IWX_TX_RING_COUNT];
	int sc_nic_locks;

	struct iwx_rx_phy_info sc_last_phy_info;
	int sc_ampdu_ref;

	uint32_t sc_time_event_uid;

	/* phy contexts.  we only use the first one */
	struct iwx_phy_ctxt sc_phyctxt[IWX_NUM_PHY_CTX];

	struct iwx_notif_statistics sc_stats;
	int sc_noise;

	int sc_pm_support;
	int sc_ltr_enabled;

	int sc_integrated;
	int sc_tx_with_siso_diversity;
	int sc_max_tfd_queue_size;
	int sc_ltr_delay;
	int sc_xtal_latency;
	int sc_low_latency_xtal;
	int sc_uhb_supported;
	int sc_umac_prph_offset;
	int sc_imr_enabled;

	struct sysctllog *sc_clog;
	kmutex_t sc_media_mtx;	/* XXX */

	struct bpf_if *sc_drvbpf;

	union {
		struct iwx_rx_radiotap_header th;
		uint8_t	pad[IEEE80211_RADIOTAP_HDRLEN];
	} sc_rxtapu;
#define sc_rxtap	sc_rxtapu.th
	int			sc_rxtap_len;

	union {
		struct iwx_tx_radiotap_header th;
		uint8_t	pad[IEEE80211_RADIOTAP_HDRLEN];
	} sc_txtapu;
#define sc_txtap	sc_txtapu.th
	int			sc_txtap_len;
};

struct iwx_node {
	struct ieee80211_node in_ni;
	struct iwx_phy_ctxt *in_phyctxt;
	uint8_t in_macaddr[ETHER_ADDR_LEN];

	uint16_t in_id;
	uint16_t in_color;

	int in_flags;
#define IWX_NODE_FLAG_HAVE_PAIRWISE_KEY		0x01
#define IWX_NODE_FLAG_HAVE_GROUP_KEY		0x02
#define IWX_NODE_FLAG_HAVE_INTEGRITY_GROUP_KEY	0x04
};

#define IWX_STATION_ID 0
#define IWX_AUX_STA_ID 1
#define IWX_MONITOR_STA_ID 2

#define IWX_ICT_SIZE		4096
#define IWX_ICT_COUNT		(IWX_ICT_SIZE / sizeof (uint32_t))
#define IWX_ICT_PADDR_SHIFT	12
