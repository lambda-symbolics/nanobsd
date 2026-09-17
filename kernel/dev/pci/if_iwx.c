/*	$NetBSD$	*/
/*	OpenBSD: if_iwx.c,v 1.229 2026/05/28 10:51:52 kirill Exp	*/

/*
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 *   Author: Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
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


/*
 * NetBSD port of the OpenBSD iwx(4) driver for Intel Wi-Fi 6 (AX200/AX201/
 * AX210/AX211) devices.  Compared to OpenBSD this port supports only legacy
 * 802.11a/b/g operation (NetBSD's net80211 has no HT/VHT support), station
 * mode only, and uses software crypto through net80211.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/queue.h>

#include <sys/cpu.h>
#include <sys/bus.h>
#include <sys/intr.h>
#include <machine/endian.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>
#include <dev/firmload.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_ether.h>

#include <netinet/in.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#define DEVNAME(_s)	device_xname((_s)->sc_dev)
#define IC2IFP(_ic_)	((_ic_)->ic_ifp)

#define le16_to_cpup(_a_) (le16toh(*(const uint16_t *)(_a_)))
#define le32_to_cpup(_a_) (le32toh(*(const uint32_t *)(_a_)))

#ifdef IWX_DEBUG
#define DPRINTF(x)	do { if (iwx_debug > 0) printf x; } while (0)
#define DPRINTFN(n, x)	do { if (iwx_debug >= (n)) printf x; } while (0)
int iwx_debug = 1;
#else
#define DPRINTF(x)	do { ; } while (0)
#define DPRINTFN(n, x)	do { ; } while (0)
#endif

#include <sys/bitops.h>
#define fls(x)	fls32(x)
#define flsl(x)	fls64(x)

#include <dev/pci/if_iwxreg.h>
#include <dev/pci/if_iwxvar.h>

/* Compatibility helpers for code shared with OpenBSD. */
#define nitems(_a)	((int)__arraycount(_a))
#define letoh16(x)	le16toh(x)
#define letoh32(x)	le32toh(x)
#define letoh64(x)	le64toh(x)
#define SEC_TO_TICKS(s)	((s) * hz)
#ifndef IEEE80211_CCMP_HDRLEN
#define IEEE80211_CCMP_HDRLEN	8
#endif

static const uint8_t iwx_etheranyaddr[ETHER_ADDR_LEN] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * PCI product IDs for devices supported by this driver, using OpenBSD's
 * naming.  Some of these are missing from NetBSD's pcidevs, so define
 * them all here.
 */
#define IWX_PCI_PRODUCT_WL_22500_1	0x2723	/* Wi-Fi 6 AX200 */
#define IWX_PCI_PRODUCT_WL_22500_2	0x02f0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_3	0xa0f0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_4	0x34f0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_5	0x06f0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_6	0x43f0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_7	0x3df0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_8	0x4df0	/* Wi-Fi 6 AX201 */
#define IWX_PCI_PRODUCT_WL_22500_9	0x2725	/* Wi-Fi 6 AX210 */
#define IWX_PCI_PRODUCT_WL_22500_10	0x2726	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_11	0x51f0	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_12	0x7a70	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_13	0x7af0	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_14	0x7e40	/* Wi-Fi 6 AX210 */
#define IWX_PCI_PRODUCT_WL_22500_15	0x7f70	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_16	0x54f0	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_17	0x51f1	/* Wi-Fi 6 AX211 */
#define IWX_PCI_PRODUCT_WL_22500_18	0x7740	/* Wi-Fi AX211 (BZ) */

static const uint8_t iwx_nvm_channels_8000[] = {
	/* 2.4 GHz */
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	/* 5 GHz */
	36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
	96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165, 169, 173, 177, 181
};

static const uint8_t iwx_nvm_channels_uhb[] = {
	/* 2.4 GHz */
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	/* 5 GHz */
	36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
	96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165, 169, 173, 177, 181,
	/* 6-7 GHz */
	1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 69,
	73, 77, 81, 85, 89, 93, 97, 101, 105, 109, 113, 117, 121, 125, 129,
	133, 137, 141, 145, 149, 153, 157, 161, 165, 169, 173, 177, 181, 185,
	189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 233
};

#define IWX_NUM_2GHZ_CHANNELS	14
#define IWX_NUM_5GHZ_CHANNELS	37

static const struct iwx_rate {
	uint16_t rate;
	uint8_t plcp;
	uint8_t ht_plcp;
} iwx_rates[] = {
		/* Legacy */		/* HT */
	{   2,	IWX_RATE_1M_PLCP,	IWX_RATE_HT_SISO_MCS_INV_PLCP  },
	{   4,	IWX_RATE_2M_PLCP,	IWX_RATE_HT_SISO_MCS_INV_PLCP },
	{  11,	IWX_RATE_5M_PLCP,	IWX_RATE_HT_SISO_MCS_INV_PLCP  },
	{  22,	IWX_RATE_11M_PLCP,	IWX_RATE_HT_SISO_MCS_INV_PLCP },
	{  12,	IWX_RATE_6M_PLCP,	IWX_RATE_HT_SISO_MCS_0_PLCP },
	{  18,	IWX_RATE_9M_PLCP,	IWX_RATE_HT_SISO_MCS_INV_PLCP  },
	{  24,	IWX_RATE_12M_PLCP,	IWX_RATE_HT_SISO_MCS_1_PLCP },
	{  26,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_8_PLCP },
	{  36,	IWX_RATE_18M_PLCP,	IWX_RATE_HT_SISO_MCS_2_PLCP },
	{  48,	IWX_RATE_24M_PLCP,	IWX_RATE_HT_SISO_MCS_3_PLCP },
	{  52,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_9_PLCP },
	{  72,	IWX_RATE_36M_PLCP,	IWX_RATE_HT_SISO_MCS_4_PLCP },
	{  78,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_10_PLCP },
	{  96,	IWX_RATE_48M_PLCP,	IWX_RATE_HT_SISO_MCS_5_PLCP },
	{ 104,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_11_PLCP },
	{ 108,	IWX_RATE_54M_PLCP,	IWX_RATE_HT_SISO_MCS_6_PLCP },
	{ 128,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_SISO_MCS_7_PLCP },
	{ 156,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_12_PLCP },
	{ 208,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_13_PLCP },
	{ 234,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_14_PLCP },
	{ 260,	IWX_RATE_INVM_PLCP,	IWX_RATE_HT_MIMO2_MCS_15_PLCP },
};
#define IWX_RIDX_CCK	0
#define IWX_RIDX_OFDM	4
#define IWX_RIDX_MAX	(nitems(iwx_rates)-1)
#define IWX_RIDX_IS_CCK(_i_) ((_i_) < IWX_RIDX_OFDM)
#define IWX_RIDX_IS_OFDM(_i_) ((_i_) >= IWX_RIDX_OFDM)
#define IWX_RVAL_IS_OFDM(_i_) ((_i_) >= 12 && (_i_) != 22)

/*
 * Default EDCA parameters (802.11-2012 Table 8-105 for OFDM PHYs).
 * NetBSD's net80211 only fills in ic_wme when IEEE80211_C_WME is
 * advertised, which we do not do, so use these for the MAC context.
 */
static const struct iwx_edca_defaults {
	uint8_t ecwmin;
	uint8_t ecwmax;
	uint8_t aifsn;
	uint16_t txop;	/* in units of 32us */
} iwx_edca_defaults[WME_NUM_AC] = {
	[WME_AC_BE] = { 4, 10, 3, 0 },
	[WME_AC_BK] = { 4, 10, 7, 0 },
	[WME_AC_VI] = { 3, 4, 2, 94 },
	[WME_AC_VO] = { 2, 3, 2, 47 },
};

/* Device configurations (see iwx_dev_info_table below). */
static const struct iwx_device_cfg iwx_9560_quz_a0_jf_b0_cfg = {
	.fw_name = IWX_QUZ_A_JF_B_FW,
};

static const struct iwx_device_cfg iwx_9560_qu_c0_jf_b0_cfg = {
	.fw_name = IWX_QU_C_JF_B_FW,
};

static const struct iwx_device_cfg iwx_qu_b0_hr1_b0 = {
	.fw_name = IWX_QU_B_HR_B_FW,
	.tx_with_siso_diversity = true,
};

static const struct iwx_device_cfg iwx_qu_b0_hr_b0 = {
	.fw_name = IWX_QU_B_HR_B_FW,
};

static const struct iwx_device_cfg iwx_qu_c0_hr1_b0 = {
	.fw_name = IWX_QU_C_HR_B_FW,
	.tx_with_siso_diversity = true,
};

static const struct iwx_device_cfg iwx_qu_c0_hr_b0 = {
	.fw_name = IWX_QU_C_HR_B_FW,
};

static const struct iwx_device_cfg iwx_quz_a0_hr1_b0 = {
	.fw_name = IWX_QUZ_A_HR_B_FW,
};

static const struct iwx_device_cfg iwx_cfg_so_a0_hr_b0 = {
	.fw_name = IWX_SO_A_HR_B_FW,
};

static const struct iwx_device_cfg iwx_cfg_quz_a0_hr_b0 = {
	.fw_name = IWX_QUZ_A_HR_B_FW,
};

static const struct iwx_device_cfg iwx_2ax_cfg_so_gf_a0 = {
	.fw_name = IWX_SO_A_GF_A_FW,
	.pnvm_name = IWX_SO_A_GF_A_PNVM,
	.uhb_supported = 1,
};

static const struct iwx_device_cfg iwx_2ax_cfg_so_gf_a0_long = {
	.fw_name = IWX_SO_A_GF_A_FW,
	.pnvm_name = IWX_SO_A_GF_A_PNVM,
	.uhb_supported = 1,
	.xtal_latency = 12000,
	.low_latency_xtal = 1,
};

static const struct iwx_device_cfg iwx_2ax_cfg_so_gf4_a0 = {
	.fw_name = IWX_SO_A_GF4_A_FW,
	.pnvm_name = IWX_SO_A_GF4_A_PNVM,
	.uhb_supported = 1,
	.xtal_latency = 12000,
	.low_latency_xtal = 1,
};

static const struct iwx_device_cfg iwx_2ax_cfg_so_gf4_a0_long = {
	.fw_name = IWX_SO_A_GF4_A_FW,
	.pnvm_name = IWX_SO_A_GF4_A_PNVM,
	.uhb_supported = 1,
};

static const struct iwx_device_cfg iwx_2ax_cfg_ty_gf_a0 = {
	.fw_name = IWX_TY_A_GF_A_FW,
	.pnvm_name = IWX_TY_A_GF_A_PNVM,
};

static const struct iwx_device_cfg iwx_2ax_cfg_so_jf_b0 = {
	.fw_name = IWX_SO_A_JF_B_FW,
};

static const struct iwx_device_cfg iwx_cfg_ma_b0_hr_b0 = {
	.fw_name = IWX_MA_B_HR_B_FW,
};

static const struct iwx_device_cfg iwx_cfg_ma_b0_gf_a0 = {
	.fw_name = IWX_MA_B_GF_A_FW,
	.pnvm_name = IWX_MA_B_GF_A_PNVM,
};

static const struct iwx_device_cfg iwx_cfg_ma_b0_gf4_a0 = {
	.fw_name = IWX_MA_B_GF4_A_FW,
	.pnvm_name = IWX_MA_B_GF4_A_PNVM,
};

static const struct iwx_device_cfg iwx_cfg_ma_a0_fm_a0 = {
	.fw_name = IWX_MA_A_FM_A_FW,
	.pnvm_name = IWX_MA_A_FM_A_PNVM,
};

/*
 * Minimal task queue implementation (see if_iwxvar.h).
 */
static void
iwx_taskq_thread(void *arg)
{
	struct iwx_taskq *tq = arg;
	struct iwx_task *t;

	for (;;) {
		mutex_enter(&tq->tq_mtx);
		while (!tq->tq_dying && TAILQ_EMPTY(&tq->tq_list))
			cv_wait(&tq->tq_cv, &tq->tq_mtx);
		if (tq->tq_dying) {
			mutex_exit(&tq->tq_mtx);
			break;
		}
		t = TAILQ_FIRST(&tq->tq_list);
		TAILQ_REMOVE(&tq->tq_list, t, t_entry);
		t->t_onqueue = 0;
		mutex_exit(&tq->tq_mtx);

		(*t->t_func)(t->t_arg);
	}

	kthread_exit(0);
}

static struct iwx_taskq *
iwx_taskq_create(const char *name)
{
	struct iwx_taskq *tq;
	int err;

	tq = kmem_zalloc(sizeof(*tq), KM_SLEEP);
	mutex_init(&tq->tq_mtx, MUTEX_DEFAULT, IPL_NET);
	cv_init(&tq->tq_cv, name);
	TAILQ_INIT(&tq->tq_list);
	strlcpy(tq->tq_name, name, sizeof(tq->tq_name));

	/* Not MPSAFE: the thread runs with the kernel lock held. */
	err = kthread_create(PRI_NONE, KTHREAD_MUSTJOIN, NULL,
	    iwx_taskq_thread, tq, &tq->tq_lwp, "%s", tq->tq_name);
	if (err) {
		cv_destroy(&tq->tq_cv);
		mutex_destroy(&tq->tq_mtx);
		kmem_free(tq, sizeof(*tq));
		return NULL;
	}

	return tq;
}

static void
iwx_taskq_destroy(struct iwx_taskq *tq)
{
	if (tq == NULL)
		return;

	mutex_enter(&tq->tq_mtx);
	tq->tq_dying = 1;
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_mtx);

	kthread_join(tq->tq_lwp);

	cv_destroy(&tq->tq_cv);
	mutex_destroy(&tq->tq_mtx);
	kmem_free(tq, sizeof(*tq));
}

static void
iwx_task_set(struct iwx_task *t, void (*fn)(void *), void *arg)
{
	memset(t, 0, sizeof(*t));
	t->t_func = fn;
	t->t_arg = arg;
}

/* Returns 1 if the task was added, 0 if it was already queued. */
static int
iwx_task_add(struct iwx_taskq *tq, struct iwx_task *t)
{
	int rv = 0;

	mutex_enter(&tq->tq_mtx);
	if (!t->t_onqueue) {
		t->t_onqueue = 1;
		TAILQ_INSERT_TAIL(&tq->tq_list, t, t_entry);
		cv_signal(&tq->tq_cv);
		rv = 1;
	}
	mutex_exit(&tq->tq_mtx);

	return rv;
}

/* Returns 1 if the task was removed from the queue, 0 otherwise. */
static int
iwx_task_del(struct iwx_taskq *tq, struct iwx_task *t)
{
	int rv = 0;

	mutex_enter(&tq->tq_mtx);
	if (t->t_onqueue) {
		TAILQ_REMOVE(&tq->tq_list, t, t_entry);
		t->t_onqueue = 0;
		rv = 1;
	}
	mutex_exit(&tq->tq_mtx);

	return rv;
}

/*
 * Reference counting of scheduled tasks; iwx_stop() waits for all
 * tasks to finish.
 */
static void
iwx_refcnt_take(struct iwx_softc *sc)
{
	sc->task_refs++;
}

static void
iwx_refcnt_rele_wake(struct iwx_softc *sc)
{
	KASSERT(sc->task_refs > 0);
	if (--sc->task_refs == 0)
		wakeup(&sc->task_refs);
}

static void
iwx_refcnt_finalize(struct iwx_softc *sc, const char *wmesg)
{
	KASSERT(sc->task_refs > 0);
	sc->task_refs--;
	while (sc->task_refs > 0)
		tsleep(&sc->task_refs, 0, wmesg, 0);
}

/* Forward declarations for functions used before their definition. */
static int	iwx_dma_contig_alloc(bus_dma_tag_t, struct iwx_dma_info *,
		    bus_size_t, bus_size_t);
static void	iwx_dma_contig_free(struct iwx_dma_info *);
static int	iwx_nic_lock(struct iwx_softc *);
static void	iwx_nic_unlock(struct iwx_softc *);
static uint32_t	iwx_read_prph(struct iwx_softc *, uint32_t);
static void	iwx_write_prph(struct iwx_softc *, uint32_t, uint32_t);
static void	iwx_write_umac_prph(struct iwx_softc *, uint32_t, uint32_t);
static int	iwx_set_bits_prph(struct iwx_softc *, uint32_t, uint32_t);
static int	iwx_clear_bits_prph(struct iwx_softc *, uint32_t, uint32_t);
static int	iwx_send_cmd(struct iwx_softc *, struct iwx_host_cmd *);
static int	iwx_send_cmd_pdu(struct iwx_softc *, uint32_t, uint32_t,
		    uint16_t, const void *);
static int	iwx_send_cmd_status(struct iwx_softc *, struct iwx_host_cmd *,
		    uint32_t *);
static int	iwx_send_cmd_pdu_status(struct iwx_softc *, uint32_t, uint16_t,
		    const void *, uint32_t *);
static void	iwx_free_resp(struct iwx_softc *, struct iwx_host_cmd *);
static int	iwx_rx_addbuf(struct iwx_softc *, int, int);
static void	iwx_update_rx_desc(struct iwx_softc *, struct iwx_rx_ring *,
		    int);
static void	iwx_reset_tx_ring(struct iwx_softc *, struct iwx_tx_ring *);
static void	iwx_free_tx_ring(struct iwx_softc *, struct iwx_tx_ring *);
static void	iwx_free_rx_ring(struct iwx_softc *, struct iwx_rx_ring *);
static void	iwx_stop_device(struct iwx_softc *);
static int	iwx_start_hw(struct iwx_softc *);
static void	iwx_ctxt_info_free_fw_img(struct iwx_softc *);
static void	iwx_ctxt_info_free_paging(struct iwx_softc *);
static void	iwx_ctxt_info_gen3_set_pnvm(struct iwx_softc *);
static void	iwx_init_channel_map(struct iwx_softc *, uint16_t *,
		    uint32_t *, int);
static int	iwx_phy_ctxt_cmd(struct iwx_softc *, struct iwx_phy_ctxt *,
		    uint8_t, uint8_t, uint32_t, uint32_t, uint8_t, uint8_t);
static int	iwx_binding_cmd(struct iwx_softc *, struct iwx_node *,
		    uint32_t);
static int	iwx_add_sta_cmd(struct iwx_softc *, struct iwx_node *, int);
static int	iwx_mld_add_sta_cmd(struct iwx_softc *, struct iwx_node *,
		    int);
static int	iwx_rm_sta_cmd(struct iwx_softc *, struct iwx_node *);
static int	iwx_mld_rm_sta_cmd(struct iwx_softc *, struct iwx_node *);
static int	iwx_mac_ctxt_cmd(struct iwx_softc *, struct iwx_node *,
		    uint32_t, int);
static int	iwx_mld_mac_ctxt_cmd(struct iwx_softc *, struct iwx_node *,
		    uint32_t, int);
static int	iwx_enable_txq(struct iwx_softc *, int, int, int, int);
static int	iwx_disable_txq(struct iwx_softc *, int, int, uint8_t);
static int	iwx_disable_mgmt_queue(struct iwx_softc *);
static int	iwx_flush_sta(struct iwx_softc *, struct iwx_node *);
static int	iwx_set_pslevel(struct iwx_softc *, int, int, int);
static int	iwx_disable_beacon_filter(struct iwx_softc *);
static int	iwx_sf_config(struct iwx_softc *, int);
static int	iwx_allow_mcast(struct iwx_softc *);
static int	iwx_clear_statistics(struct iwx_softc *);
static int	iwx_scan_abort(struct iwx_softc *);
static int	iwx_rs_init(struct iwx_softc *, struct iwx_node *);
static int	iwx_phy_send_rlc(struct iwx_softc *, struct iwx_phy_ctxt *,
		    uint8_t, uint8_t);
static uint8_t	iwx_fw_valid_tx_ant(struct iwx_softc *);
static uint8_t	iwx_fw_valid_rx_ant(struct iwx_softc *);
static void	iwx_ack_rates(struct iwx_softc *, struct iwx_node *, int *,
		    int *);
static void	iwx_endscan(struct iwx_softc *);
static void	iwx_nic_error(struct iwx_softc *);
static void	iwx_dump_driver_status(struct iwx_softc *);
static void	iwx_txq_advance(struct iwx_softc *, struct iwx_tx_ring *,
		    uint16_t);
static void	iwx_tx_update_byte_tbl(struct iwx_softc *, struct iwx_tx_ring *,
		    int, uint16_t, uint16_t);
static int	iwx_init(struct ifnet *);
static int	iwx_init_locked(struct ifnet *);
static void	iwx_stop(struct ifnet *, int);
static void	iwx_stop_locked(struct ifnet *);
static void	iwx_start(struct ifnet *);
static void	iwx_watchdog(struct ifnet *);
static int	iwx_ioctl(struct ifnet *, u_long, void *);
static int	iwx_preinit(struct iwx_softc *);
static void	iwx_attach_hook(device_t);
static void	iwx_init_task(void *);
static void	iwx_newstate_task(void *);
static int	iwx_newstate(struct ieee80211com *, enum ieee80211_state, int);
static int	iwx_media_change(struct ifnet *);
static void	iwx_radiotap_attach(struct iwx_softc *);
static int	iwx_match(device_t, cfdata_t, void *);
static void	iwx_attach(device_t, device_t, void *);
static int	iwx_detach(device_t, int);

static uint8_t
iwx_lookup_cmd_ver(struct iwx_softc *sc, uint8_t grp, uint8_t cmd)
{
	const struct iwx_fw_cmd_version *entry;
	int i;

	for (i = 0; i < sc->n_cmd_versions; i++) {
		entry = &sc->cmd_versions[i];
		if (entry->group == grp && entry->cmd == cmd)
			return entry->cmd_ver;
	}

	return IWX_FW_CMD_VER_UNKNOWN;
}

static uint8_t
iwx_lookup_notif_ver(struct iwx_softc *sc, uint8_t grp, uint8_t cmd)
{
	const struct iwx_fw_cmd_version *entry;
	int i;

	for (i = 0; i < sc->n_cmd_versions; i++) {
		entry = &sc->cmd_versions[i];
		if (entry->group == grp && entry->cmd == cmd)
			return entry->notif_ver;
	}

	return IWX_FW_CMD_VER_UNKNOWN;
}

static int
iwx_store_cscheme(struct iwx_softc *sc, uint8_t *data, size_t dlen)
{
	struct iwx_fw_cscheme_list *l = (void *)data;

	if (dlen < sizeof(*l) ||
	    dlen < sizeof(l->size) + l->size * sizeof(*l->cs))
		return EINVAL;

	/* we don't actually store anything for now, always use s/w crypto */

	return 0;
}

static int
iwx_ctxt_info_alloc_dma(struct iwx_softc *sc,
    const struct iwx_fw_onesect *sec, struct iwx_dma_info *dram)
{
	int err = iwx_dma_contig_alloc(sc->sc_dmat, dram, sec->fws_len, 0);
	if (err) {
		printf("%s: could not allocate context info DMA memory\n",
		    DEVNAME(sc));
		return err;
	}

	memcpy(dram->vaddr, sec->fws_data, sec->fws_len);

	return 0;
}

static void
iwx_ctxt_info_free_paging(struct iwx_softc *sc)
{
	struct iwx_self_init_dram *dram = &sc->init_dram;
	int i;

	if (!dram->paging)
		return;

	/* free paging*/
	for (i = 0; i < dram->paging_cnt; i++)
		iwx_dma_contig_free(&dram->paging[i]);

	kmem_free(dram->paging, dram->paging_cnt * sizeof(*dram->paging));
	dram->paging_cnt = 0;
	dram->paging = NULL;
}

static int
iwx_get_num_sections(const struct iwx_fw_sects *fws, int start)
{
	int i = 0;

	while (start < fws->fw_count &&
	       fws->fw_sect[start].fws_devoff != IWX_CPU1_CPU2_SEPARATOR_SECTION &&
	       fws->fw_sect[start].fws_devoff != IWX_PAGING_SEPARATOR_SECTION) {
		start++;
		i++;
	}

	return i;
}

static int
iwx_init_fw_sec(struct iwx_softc *sc, const struct iwx_fw_sects *fws,
    struct iwx_context_info_dram *ctxt_dram)
{
	struct iwx_self_init_dram *dram = &sc->init_dram;
	int i, ret, fw_cnt = 0;

	KASSERT(dram->paging == NULL);

	dram->lmac_cnt = iwx_get_num_sections(fws, 0);
	/* add 1 due to separator */
	dram->umac_cnt = iwx_get_num_sections(fws, dram->lmac_cnt + 1);
	/* add 2 due to separators */
	dram->paging_cnt = iwx_get_num_sections(fws,
	    dram->lmac_cnt + dram->umac_cnt + 2);

	dram->fw = kmem_zalloc((dram->umac_cnt + dram->lmac_cnt) *
	    sizeof(*dram->fw), KM_SLEEP);
	if (!dram->fw) {
		printf("%s: could not allocate memory for firmware sections\n",
		    DEVNAME(sc));
		return ENOMEM;
	}

	if (dram->paging_cnt > 0) {
		dram->paging = kmem_zalloc(dram->paging_cnt *
		    sizeof(*dram->paging), KM_SLEEP);
		if (!dram->paging) {
			printf("%s: could not allocate memory for firmware "
			    "paging\n", DEVNAME(sc));
			return ENOMEM;
		}
	}

	/* initialize lmac sections */
	for (i = 0; i < dram->lmac_cnt; i++) {
		ret = iwx_ctxt_info_alloc_dma(sc, &fws->fw_sect[i],
						   &dram->fw[fw_cnt]);
		if (ret)
			return ret;
		ctxt_dram->lmac_img[i] =
			htole64(dram->fw[fw_cnt].paddr);
		DPRINTF(("%s: firmware LMAC section %d at 0x%llx size %lld\n", __func__, i,
		    (unsigned long long)dram->fw[fw_cnt].paddr,
		    (unsigned long long)dram->fw[fw_cnt].size));
		fw_cnt++;
	}

	/* initialize umac sections */
	for (i = 0; i < dram->umac_cnt; i++) {
		/* access FW with +1 to make up for lmac separator */
		ret = iwx_ctxt_info_alloc_dma(sc,
		    &fws->fw_sect[fw_cnt + 1], &dram->fw[fw_cnt]);
		if (ret)
			return ret;
		ctxt_dram->umac_img[i] =
			htole64(dram->fw[fw_cnt].paddr);
		DPRINTF(("%s: firmware UMAC section %d at 0x%llx size %lld\n", __func__, i,
			(unsigned long long)dram->fw[fw_cnt].paddr,
			(unsigned long long)dram->fw[fw_cnt].size));
		fw_cnt++;
	}

	/*
	 * Initialize paging.
	 * Paging memory isn't stored in dram->fw as the umac and lmac - it is
	 * stored separately.
	 * This is since the timing of its release is different -
	 * while fw memory can be released on alive, the paging memory can be
	 * freed only when the device goes down.
	 * Given that, the logic here in accessing the fw image is a bit
	 * different - fw_cnt isn't changing so loop counter is added to it.
	 */
	for (i = 0; i < dram->paging_cnt; i++) {
		/* access FW with +2 to make up for lmac & umac separators */
		int fw_idx = fw_cnt + i + 2;

		ret = iwx_ctxt_info_alloc_dma(sc,
		    &fws->fw_sect[fw_idx], &dram->paging[i]);
		if (ret)
			return ret;

		ctxt_dram->virtual_img[i] = htole64(dram->paging[i].paddr);
		DPRINTF(("%s: firmware paging section %d at 0x%llx size %lld\n", __func__, i,
		    (unsigned long long)dram->paging[i].paddr,
		    (unsigned long long)dram->paging[i].size));
	}

	return 0;
}

static void
iwx_fw_version_str(char *buf, size_t bufsize,
    uint32_t major, uint32_t minor, uint32_t api)
{
	/*
	 * Starting with major version 35 the Linux driver prints the minor
	 * version in hexadecimal.
	 */
	if (major >= 35)
		snprintf(buf, bufsize, "%u.%08x.%u", major, minor, api);
	else
		snprintf(buf, bufsize, "%u.%u.%u", major, minor, api);
}

static int
iwx_alloc_fw_monitor_block(struct iwx_softc *sc, uint8_t max_power,
    uint8_t min_power)
{
	struct iwx_dma_info *fw_mon = &sc->fw_mon;
	uint32_t size = 0;
	uint8_t power;
	int err = 0;

	if (fw_mon->size)
		return 0;

	for (power = max_power; power >= min_power; power--) {
		size = (1 << power);

		err = iwx_dma_contig_alloc(sc->sc_dmat, fw_mon, size, 0);
		if (err)
			continue;

		DPRINTF(("%s: allocated 0x%08x bytes for firmware monitor.\n",
			 DEVNAME(sc), size));
		break;
	}

	if (err) {
		fw_mon->size = 0;
		return err;
	}

	if (power != max_power)
		DPRINTF(("%s: Sorry - debug buffer is only %luK while you requested %luK\n",
			DEVNAME(sc), (unsigned long)(1 << (power - 10)),
			(unsigned long)(1 << (max_power - 10))));

	return 0;
}

static int
iwx_alloc_fw_monitor(struct iwx_softc *sc, uint8_t max_power)
{
	if (!max_power) {
		/* default max_power is maximum */
		max_power = 26;
	} else {
		max_power += 11;
	}

	if (max_power > 26) {
		DPRINTF(("%s: External buffer size for monitor is too big %d, "
		     "check the FW TLV\n", DEVNAME(sc), max_power));
		return 0;
	}

	if (sc->fw_mon.size)
		return 0;

	return iwx_alloc_fw_monitor_block(sc, max_power, 11);
}

static int
iwx_apply_debug_destination(struct iwx_softc *sc)
{
	struct iwx_fw_dbg_dest_tlv_v1 *dest_v1;
	int i, err;
	uint8_t mon_mode, size_power, base_shift, end_shift;
	uint32_t base_reg, end_reg;

	dest_v1 = sc->sc_fw.dbg_dest_tlv_v1;
	mon_mode = dest_v1->monitor_mode;
	size_power = dest_v1->size_power;
	base_reg = le32toh(dest_v1->base_reg);
	end_reg = le32toh(dest_v1->end_reg);
	base_shift = dest_v1->base_shift;
	end_shift = dest_v1->end_shift;

	DPRINTF(("%s: applying debug destination %d\n", DEVNAME(sc), mon_mode));

	if (mon_mode == EXTERNAL_MODE) {
		err = iwx_alloc_fw_monitor(sc, size_power);
		if (err)
			return err;
	}

	if (!iwx_nic_lock(sc))
		return EBUSY;

	for (i = 0; i < sc->sc_fw.n_dest_reg; i++) {
		uint32_t addr, val;
		uint8_t op;

		addr = le32toh(dest_v1->reg_ops[i].addr);
		val = le32toh(dest_v1->reg_ops[i].val);
		op = dest_v1->reg_ops[i].op;

		DPRINTF(("%s: op=%u addr=%u val=%u\n", __func__, op, addr, val));
		switch (op) {
		case CSR_ASSIGN:
			IWX_WRITE(sc, addr, val);
			break;
		case CSR_SETBIT:
			IWX_SETBITS(sc, addr, (1 << val));
			break;
		case CSR_CLEARBIT:
			IWX_CLRBITS(sc, addr, (1 << val));
			break;
		case PRPH_ASSIGN:
			iwx_write_prph(sc, addr, val);
			break;
		case PRPH_SETBIT:
			err = iwx_set_bits_prph(sc, addr, (1 << val));
			if (err) {
				iwx_nic_unlock(sc);
				return err;
			}
			break;
		case PRPH_CLEARBIT:
			err = iwx_clear_bits_prph(sc, addr, (1 << val));
			if (err) {
				iwx_nic_unlock(sc);
				return err;
			}
			break;
		case PRPH_BLOCKBIT:
			if (iwx_read_prph(sc, addr) & (1 << val))
				goto monitor;
			break;
		default:
			DPRINTF(("%s: FW debug - unknown OP %d\n",
			    DEVNAME(sc), op));
			break;
		}
	}

monitor:
	if (mon_mode == EXTERNAL_MODE && sc->fw_mon.size) {
		iwx_write_prph(sc, le32toh(base_reg),
		    sc->fw_mon.paddr >> base_shift);
		iwx_write_prph(sc, end_reg,
		    (sc->fw_mon.paddr + sc->fw_mon.size - 256)
		    >> end_shift);
	}

	iwx_nic_unlock(sc);
	return 0;
}

static void
iwx_set_ltr(struct iwx_softc *sc)
{
	uint32_t ltr_val = IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ |
	    ((IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC <<
	    IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_SHIFT) &
	    IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_MASK) |
	    ((250 << IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_SHIFT) &
	    IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_MASK) |
	    IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ |
	    ((IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC <<
	    IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_SHIFT) &
	    IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_MASK) |
	    (250 & IWX_CSR_LTR_LONG_VAL_AD_SNOOP_VAL);

	/*
	 * To workaround hardware latency issues during the boot process,
	 * initialize the LTR to ~250 usec (see ltr_val above).
	 * The firmware initializes this again later (to a smaller value).
	 */
	if (!sc->sc_integrated) {
		IWX_WRITE(sc, IWX_CSR_LTR_LONG_VAL_AD, ltr_val);
	} else if (sc->sc_integrated &&
		   sc->sc_device_family == IWX_DEVICE_FAMILY_22000) {
		iwx_write_prph(sc, IWX_HPM_MAC_LTR_CSR,
		    IWX_HPM_MAC_LRT_ENABLE_ALL);
		iwx_write_prph(sc, IWX_HPM_UMAC_LTR, ltr_val);
	}
}

static int
iwx_ctxt_info_init(struct iwx_softc *sc, const struct iwx_fw_sects *fws)
{
	struct iwx_context_info *ctxt_info;
	struct iwx_context_info_rbd_cfg *rx_cfg;
	uint32_t control_flags = 0;
	uint64_t paddr;
	int err;

	ctxt_info = sc->ctxt_info_dma.vaddr;
	memset(ctxt_info, 0, sizeof(*ctxt_info));

	ctxt_info->version.version = 0;
	ctxt_info->version.mac_id =
		htole16((uint16_t)IWX_READ(sc, IWX_CSR_HW_REV));

	/* size is in DWs */
	ctxt_info->version.size = htole16(sizeof(*ctxt_info) / 4);

	KASSERT(IWX_RX_QUEUE_CB_SIZE(IWX_MQ_RX_TABLE_SIZE) < 0xF);
	control_flags = IWX_CTXT_INFO_TFD_FORMAT_LONG |
			(IWX_RX_QUEUE_CB_SIZE(IWX_MQ_RX_TABLE_SIZE) <<
			 IWX_CTXT_INFO_RB_CB_SIZE_POS) |
			(IWX_CTXT_INFO_RB_SIZE_4K << IWX_CTXT_INFO_RB_SIZE_POS);
	ctxt_info->control.control_flags = htole32(control_flags);

	/* initialize RX default queue */
	rx_cfg = &ctxt_info->rbd_cfg;
	rx_cfg->free_rbd_addr = htole64(sc->rxq.free_desc_dma.paddr);
	rx_cfg->used_rbd_addr = htole64(sc->rxq.used_desc_dma.paddr);
	rx_cfg->status_wr_ptr = htole64(sc->rxq.stat_dma.paddr);

	/* initialize TX command queue */
	ctxt_info->hcmd_cfg.cmd_queue_addr =
	    htole64(sc->txq[IWX_DQA_CMD_QUEUE].desc_dma.paddr);
	ctxt_info->hcmd_cfg.cmd_queue_size =
		IWX_TFD_QUEUE_CB_SIZE(IWX_TX_RING_COUNT);

	/* allocate ucode sections in dram and set addresses */
	err = iwx_init_fw_sec(sc, fws, &ctxt_info->dram);
	if (err) {
		iwx_ctxt_info_free_fw_img(sc);
		return err;
	}

	/* Configure debug, if exists */
	if (sc->sc_fw.dbg_dest_tlv_v1) {
		err = iwx_apply_debug_destination(sc);
		if (err) {
			iwx_ctxt_info_free_fw_img(sc);
			return err;
		}
	}

	/*
	 * Write the context info DMA base address. The device expects a
	 * 64-bit address but a simple bus_space_write_8 to this register
	 * won't work on some devices, such as the AX201.
	 */
	paddr = sc->ctxt_info_dma.paddr;
	IWX_WRITE(sc, IWX_CSR_CTXT_INFO_BA, paddr & 0xffffffff);
	IWX_WRITE(sc, IWX_CSR_CTXT_INFO_BA + 4, paddr >> 32);

	/* kick FW self load */
	if (!iwx_nic_lock(sc)) {
		iwx_ctxt_info_free_fw_img(sc);
		return EBUSY;
	}

	iwx_set_ltr(sc);
	iwx_write_prph(sc, IWX_UREG_CPU_INIT_RUN, 1);
	iwx_nic_unlock(sc);

	/* Context info will be released upon alive or failure to get one */

	return 0;
}

static int
iwx_ctxt_info_gen3_init(struct iwx_softc *sc, const struct iwx_fw_sects *fws)
{
	struct iwx_context_info_gen3 *ctxt_info_gen3;
	struct iwx_prph_scratch *prph_scratch;
	struct iwx_prph_scratch_ctrl_cfg *prph_sc_ctrl;
	uint16_t cb_size;
	uint32_t control_flags, scratch_size;
	uint64_t paddr;
	int err;

	if (sc->sc_fw.iml == NULL || sc->sc_fw.iml_len == 0) {
		printf("%s: no image loader found in firmware file\n",
		    DEVNAME(sc));
		iwx_ctxt_info_free_fw_img(sc);
		return EINVAL;
	}

	err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->iml_dma,
	    sc->sc_fw.iml_len, 0);
	if (err) {
		printf("%s: could not allocate DMA memory for "
		    "firmware image loader\n", DEVNAME(sc));
		iwx_ctxt_info_free_fw_img(sc);
		return ENOMEM;
	}

	prph_scratch = sc->prph_scratch_dma.vaddr;
	memset(prph_scratch, 0, sizeof(*prph_scratch));
	prph_sc_ctrl = &prph_scratch->ctrl_cfg;
	prph_sc_ctrl->version.version = 0;
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		prph_sc_ctrl->version.mac_id = htole16(sc->sc_hw_rev);
	else {
		prph_sc_ctrl->version.mac_id = htole16(IWX_READ(sc,
		    IWX_CSR_HW_REV));
	}
	prph_sc_ctrl->version.size = htole16(sizeof(*prph_scratch) / 4);

	control_flags = IWX_PRPH_SCRATCH_RB_SIZE_4K |
	    IWX_PRPH_SCRATCH_MTR_MODE |
	    (IWX_PRPH_MTR_FORMAT_256B & IWX_PRPH_SCRATCH_MTR_FORMAT);
	if (sc->sc_imr_enabled)
		control_flags |= IWX_PRPH_SCRATCH_IMR_DEBUG_EN;
	prph_sc_ctrl->control.control_flags = htole32(control_flags);

	/* initialize RX default queue */
	prph_sc_ctrl->rbd_cfg.free_rbd_addr =
	    htole64(sc->rxq.free_desc_dma.paddr);

	/* allocate ucode sections in dram and set addresses */
	err = iwx_init_fw_sec(sc, fws, &prph_scratch->dram);
	if (err) {
		iwx_dma_contig_free(&sc->iml_dma);
		iwx_ctxt_info_free_fw_img(sc);
		return err;
	}

	ctxt_info_gen3 = sc->ctxt_info_dma.vaddr;
	memset(ctxt_info_gen3, 0, sizeof(*ctxt_info_gen3));
	ctxt_info_gen3->prph_info_base_addr = htole64(sc->prph_info_dma.paddr);
	ctxt_info_gen3->prph_scratch_base_addr =
	    htole64(sc->prph_scratch_dma.paddr);
	scratch_size = sizeof(*prph_scratch);
	ctxt_info_gen3->prph_scratch_size = htole32(scratch_size);
	ctxt_info_gen3->cr_head_idx_arr_base_addr =
	    htole64(sc->rxq.stat_dma.paddr);
	ctxt_info_gen3->tr_tail_idx_arr_base_addr =
	    htole64(sc->prph_info_dma.paddr + PAGE_SIZE / 2);
	ctxt_info_gen3->cr_tail_idx_arr_base_addr =
	    htole64(sc->prph_info_dma.paddr + 3 * PAGE_SIZE / 4);
	ctxt_info_gen3->mtr_base_addr =
	    htole64(sc->txq[IWX_DQA_CMD_QUEUE].desc_dma.paddr);
	ctxt_info_gen3->mcr_base_addr = htole64(sc->rxq.used_desc_dma.paddr);
	cb_size = IWX_TFD_QUEUE_CB_SIZE(IWX_TX_RING_COUNT);
	ctxt_info_gen3->mtr_size = htole16(cb_size);
	cb_size = IWX_RX_QUEUE_CB_SIZE(IWX_MQ_RX_TABLE_SIZE);
	ctxt_info_gen3->mcr_size = htole16(cb_size);

	memcpy(sc->iml_dma.vaddr, sc->sc_fw.iml, sc->sc_fw.iml_len);

	paddr = sc->ctxt_info_dma.paddr;
	IWX_WRITE(sc, IWX_CSR_CTXT_INFO_ADDR, paddr & 0xffffffff);
	IWX_WRITE(sc, IWX_CSR_CTXT_INFO_ADDR + 4, paddr >> 32);

	paddr = sc->iml_dma.paddr;
	IWX_WRITE(sc, IWX_CSR_IML_DATA_ADDR, paddr & 0xffffffff);
	IWX_WRITE(sc, IWX_CSR_IML_DATA_ADDR + 4, paddr >> 32);
	IWX_WRITE(sc, IWX_CSR_IML_SIZE_ADDR, sc->sc_fw.iml_len);

	IWX_SETBITS(sc, IWX_CSR_CTXT_INFO_BOOT_CTRL,
		    IWX_CSR_AUTO_FUNC_BOOT_ENA);

	/* kick FW self load */
	if (!iwx_nic_lock(sc)) {
		iwx_dma_contig_free(&sc->iml_dma);
		iwx_ctxt_info_free_fw_img(sc);
		return EBUSY;
	}
	iwx_set_ltr(sc);
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_WRITE(sc, IWX_CSR_FUNC_SCRATCH,
		    IWX_CSR_FUNC_SCRATCH_INIT_VALUE);
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_ROM_START);
	} else
		iwx_write_umac_prph(sc, IWX_UREG_CPU_INIT_RUN, 1);
	iwx_nic_unlock(sc);

	/* Context info will be released upon alive or failure to get one */
	return 0;
}

static void
iwx_ctxt_info_free_fw_img(struct iwx_softc *sc)
{
	struct iwx_self_init_dram *dram = &sc->init_dram;
	int i;

	if (!dram->fw)
		return;

	for (i = 0; i < dram->lmac_cnt + dram->umac_cnt; i++)
		iwx_dma_contig_free(&dram->fw[i]);

	kmem_free(dram->fw,
	    (dram->lmac_cnt + dram->umac_cnt) * sizeof(*dram->fw));
	dram->lmac_cnt = 0;
	dram->umac_cnt = 0;
	dram->fw = NULL;
}

static int
iwx_firmware_store_section(struct iwx_softc *sc, enum iwx_ucode_type type,
    uint8_t *data, size_t dlen)
{
	struct iwx_fw_sects *fws;
	struct iwx_fw_onesect *fwone;

	if (type >= IWX_UCODE_TYPE_MAX)
		return EINVAL;
	if (dlen < sizeof(uint32_t))
		return EINVAL;

	fws = &sc->sc_fw.fw_sects[type];
	DPRINTF(("%s: ucode type %d section %d\n", DEVNAME(sc), type, fws->fw_count));
	if (fws->fw_count >= IWX_UCODE_SECT_MAX)
		return EINVAL;

	fwone = &fws->fw_sect[fws->fw_count];

	/* first 32bit are device load offset */
	memcpy(&fwone->fws_devoff, data, sizeof(uint32_t));

	/* rest is data */
	fwone->fws_data = data + sizeof(uint32_t);
	fwone->fws_len = dlen - sizeof(uint32_t);

	fws->fw_count++;
	fws->fw_totlen += fwone->fws_len;

	return 0;
}

#define IWX_DEFAULT_SCAN_CHANNELS	40
/* Newer firmware might support more channels. Raise this value if needed. */
#define IWX_MAX_SCAN_CHANNELS		67 /* as of iwx-cc-a0-62 firmware */

struct iwx_tlv_calib_data {
	uint32_t ucode_type;
	struct iwx_tlv_calib_ctrl calib;
} __packed;

static int
iwx_set_default_calib(struct iwx_softc *sc, const void *data)
{
	const struct iwx_tlv_calib_data *def_calib = data;
	uint32_t ucode_type = le32toh(def_calib->ucode_type);

	if (ucode_type >= IWX_UCODE_TYPE_MAX)
		return EINVAL;

	sc->sc_default_calib[ucode_type].flow_trigger =
	    def_calib->calib.flow_trigger;
	sc->sc_default_calib[ucode_type].event_trigger =
	    def_calib->calib.event_trigger;

	return 0;
}

static void
iwx_fw_info_free(struct iwx_fw_info *fw)
{
	if (fw->fw_rawdata != NULL)
		kmem_free(fw->fw_rawdata, fw->fw_rawsize);
	fw->fw_rawdata = NULL;
	fw->fw_rawsize = 0;
	/* don't touch fw->fw_status */
	memset(fw->fw_sects, 0, sizeof(fw->fw_sects));
	if (fw->iml != NULL)
		kmem_free(fw->iml, fw->iml_len);
	fw->iml = NULL;
	fw->iml_len = 0;
	if (fw->pnvm != NULL)
		kmem_free(fw->pnvm, fw->pnvm_len);
	fw->pnvm = NULL;
	fw->pnvm_len = 0;
	fw->dbg_dest_tlv_init = 0;
	fw->dbg_dest_tlv_v1 = NULL;
	fw->dbg_dest_ver = NULL;
	fw->n_dest_reg = 0;
	memset(fw->dbg_conf_tlv, 0, sizeof(fw->dbg_conf_tlv));
	memset(fw->dbg_conf_tlv_len, 0, sizeof(fw->dbg_conf_tlv_len));
}

/*
 * Load the firmware image from the filesystem into memory.
 */
static int
iwx_firmload(struct iwx_softc *sc)
{
	struct iwx_fw_info *fw = &sc->sc_fw;
	firmware_handle_t fwh;
	int err;

	err = firmware_open("if_iwx", sc->sc_fwname, &fwh);
	if (err) {
		printf("%s: could not read firmware %s (error %d)\n",
		    DEVNAME(sc), sc->sc_fwname, err);
		return err;
	}

	fw->fw_rawsize = firmware_get_size(fwh);
	if (fw->fw_rawsize < sizeof(struct iwx_tlv_ucode_header)) {
		printf("%s: firmware %s too short: %zu bytes\n",
		    DEVNAME(sc), sc->sc_fwname, fw->fw_rawsize);
		firmware_close(fwh);
		return EINVAL;
	}

	fw->fw_rawdata = kmem_alloc(fw->fw_rawsize, KM_SLEEP);
	err = firmware_read(fwh, 0, fw->fw_rawdata, fw->fw_rawsize);
	firmware_close(fwh);
	if (err) {
		printf("%s: could not read firmware %s (error %d)\n",
		    DEVNAME(sc), sc->sc_fwname, err);
		kmem_free(fw->fw_rawdata, fw->fw_rawsize);
		fw->fw_rawdata = NULL;
		fw->fw_rawsize = 0;
		return err;
	}

	return 0;
}

#define IWX_FW_ADDR_CACHE_CONTROL 0xC0000000

static int
iwx_read_firmware(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_fw_info *fw = &sc->sc_fw;
	struct iwx_tlv_ucode_header *uhdr;
	struct iwx_ucode_tlv tlv;
	uint32_t tlv_type = 0;
	uint8_t *data;
	int err;
	size_t len;

	if (fw->fw_status == IWX_FW_STATUS_DONE)
		return 0;

	while (fw->fw_status == IWX_FW_STATUS_INPROGRESS)
		tsleep(&sc->sc_fw, 0, "iwxfwp", 0);
	fw->fw_status = IWX_FW_STATUS_INPROGRESS;

	if (fw->fw_rawdata != NULL)
		iwx_fw_info_free(fw);

	err = iwx_firmload(sc);
	if (err)
		goto out;

	if (ic->ic_ifp != NULL && (ic->ic_ifp->if_flags & IFF_DEBUG))
		printf("%s: using firmware %s\n", DEVNAME(sc), sc->sc_fwname);

	sc->sc_capaflags = 0;
	sc->sc_capa_n_scan_channels = IWX_DEFAULT_SCAN_CHANNELS;
	memset(sc->sc_enabled_capa, 0, sizeof(sc->sc_enabled_capa));
	memset(sc->sc_ucode_api, 0, sizeof(sc->sc_ucode_api));
	sc->n_cmd_versions = 0;

	uhdr = (void *)fw->fw_rawdata;
	if (*(uint32_t *)fw->fw_rawdata != 0
	    || le32toh(uhdr->magic) != IWX_TLV_UCODE_MAGIC) {
		printf("%s: invalid firmware %s\n",
		    DEVNAME(sc), sc->sc_fwname);
		err = EINVAL;
		goto out;
	}

	iwx_fw_version_str(sc->sc_fwver, sizeof(sc->sc_fwver),
	    IWX_UCODE_MAJOR(le32toh(uhdr->ver)),
	    IWX_UCODE_MINOR(le32toh(uhdr->ver)),
	    IWX_UCODE_API(le32toh(uhdr->ver)));

	data = uhdr->data;
	len = fw->fw_rawsize - sizeof(*uhdr);

	while (len >= sizeof(tlv)) {
		size_t tlv_len;
		void *tlv_data;

		memcpy(&tlv, data, sizeof(tlv));
		tlv_len = le32toh(tlv.length);
		tlv_type = le32toh(tlv.type);

		len -= sizeof(tlv);
		data += sizeof(tlv);
		tlv_data = data;

		if (len < tlv_len) {
			printf("%s: firmware too short: %zu bytes\n",
			    DEVNAME(sc), len);
			err = EINVAL;
			goto parse_out;
		}

		switch (tlv_type) {
		case IWX_UCODE_TLV_PROBE_MAX_LEN:
			if (tlv_len < sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			sc->sc_capa_max_probe_len
			    = le32toh(*(uint32_t *)tlv_data);
			if (sc->sc_capa_max_probe_len >
			    IWX_SCAN_OFFLOAD_PROBE_REQ_SIZE) {
				err = EINVAL;
				goto parse_out;
			}
			break;
		case IWX_UCODE_TLV_PAN:
			if (tlv_len) {
				err = EINVAL;
				goto parse_out;
			}
			sc->sc_capaflags |= IWX_UCODE_TLV_FLAGS_PAN;
			break;
		case IWX_UCODE_TLV_FLAGS:
			if (tlv_len < sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			/*
			 * Apparently there can be many flags, but Linux driver
			 * parses only the first one, and so do we.
			 *
			 * XXX: why does this override IWX_UCODE_TLV_PAN?
			 * Intentional or a bug?  Observations from
			 * current firmware file:
			 *  1) TLV_PAN is parsed first
			 *  2) TLV_FLAGS contains TLV_FLAGS_PAN
			 * ==> this resets TLV_PAN to itself... hnnnk
			 */
			sc->sc_capaflags = le32toh(*(uint32_t *)tlv_data);
			break;
		case IWX_UCODE_TLV_CSCHEME:
			err = iwx_store_cscheme(sc, tlv_data, tlv_len);
			if (err)
				goto parse_out;
			break;
		case IWX_UCODE_TLV_NUM_OF_CPU: {
			uint32_t num_cpu;
			if (tlv_len != sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			num_cpu = le32toh(*(uint32_t *)tlv_data);
			if (num_cpu < 1 || num_cpu > 2) {
				err = EINVAL;
				goto parse_out;
			}
			break;
		}
		case IWX_UCODE_TLV_SEC_RT:
			err = iwx_firmware_store_section(sc,
			    IWX_UCODE_TYPE_REGULAR, tlv_data, tlv_len);
			if (err)
				goto parse_out;
			break;
		case IWX_UCODE_TLV_SEC_INIT:
			err = iwx_firmware_store_section(sc,
			    IWX_UCODE_TYPE_INIT, tlv_data, tlv_len);
			if (err)
				goto parse_out;
			break;
		case IWX_UCODE_TLV_SEC_WOWLAN:
			err = iwx_firmware_store_section(sc,
			    IWX_UCODE_TYPE_WOW, tlv_data, tlv_len);
			if (err)
				goto parse_out;
			break;
		case IWX_UCODE_TLV_DEF_CALIB:
			if (tlv_len != sizeof(struct iwx_tlv_calib_data)) {
				err = EINVAL;
				goto parse_out;
			}
			err = iwx_set_default_calib(sc, tlv_data);
			if (err)
				goto parse_out;
			break;
		case IWX_UCODE_TLV_PHY_SKU:
			if (tlv_len != sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			sc->sc_fw_phy_config = le32toh(*(uint32_t *)tlv_data);
			break;

		case IWX_UCODE_TLV_API_CHANGES_SET: {
			struct iwx_ucode_api *api;
			int idx, i;
			if (tlv_len != sizeof(*api)) {
				err = EINVAL;
				goto parse_out;
			}
			api = (struct iwx_ucode_api *)tlv_data;
			idx = le32toh(api->api_index);
			if (idx >= howmany(IWX_NUM_UCODE_TLV_API, 32)) {
				err = EINVAL;
				goto parse_out;
			}
			for (i = 0; i < 32; i++) {
				if ((le32toh(api->api_flags) & (1 << i)) == 0)
					continue;
				setbit(sc->sc_ucode_api, i + (32 * idx));
			}
			break;
		}

		case IWX_UCODE_TLV_ENABLED_CAPABILITIES: {
			struct iwx_ucode_capa *capa;
			int idx, i;
			if (tlv_len != sizeof(*capa)) {
				err = EINVAL;
				goto parse_out;
			}
			capa = (struct iwx_ucode_capa *)tlv_data;
			idx = le32toh(capa->api_index);
			if (idx >= howmany(IWX_NUM_UCODE_TLV_CAPA, 32)) {
				err = EINVAL;
				goto parse_out;
			}
			for (i = 0; i < 32; i++) {
				if ((le32toh(capa->api_capa) & (1 << i)) == 0)
					continue;
				setbit(sc->sc_enabled_capa, i + (32 * idx));
			}
			break;
		}

		case IWX_UCODE_TLV_SDIO_ADMA_ADDR:
		case IWX_UCODE_TLV_FW_GSCAN_CAPA:
			/* ignore, not used by current driver */
			break;

		case IWX_UCODE_TLV_SEC_RT_USNIFFER:
			err = iwx_firmware_store_section(sc,
			    IWX_UCODE_TYPE_REGULAR_USNIFFER, tlv_data,
			    tlv_len);
			if (err)
				goto parse_out;
			break;

		case IWX_UCODE_TLV_PAGING:
			if (tlv_len != sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			break;

		case IWX_UCODE_TLV_N_SCAN_CHANNELS:
			if (tlv_len != sizeof(uint32_t)) {
				err = EINVAL;
				goto parse_out;
			}
			sc->sc_capa_n_scan_channels =
			  le32toh(*(uint32_t *)tlv_data);
			if (sc->sc_capa_n_scan_channels > IWX_MAX_SCAN_CHANNELS) {
				err = ERANGE;
				goto parse_out;
			}
			break;

		case IWX_UCODE_TLV_FW_VERSION:
			if (tlv_len != sizeof(uint32_t) * 3) {
				err = EINVAL;
				goto parse_out;
			}

			iwx_fw_version_str(sc->sc_fwver, sizeof(sc->sc_fwver),
			    le32toh(((uint32_t *)tlv_data)[0]),
			    le32toh(((uint32_t *)tlv_data)[1]),
			    le32toh(((uint32_t *)tlv_data)[2]));
			break;

		case IWX_UCODE_TLV_FW_DBG_DEST: {
			struct iwx_fw_dbg_dest_tlv_v1 *dest_v1 = NULL;

			fw->dbg_dest_ver = (uint8_t *)tlv_data;
			if (*fw->dbg_dest_ver != 0) {
				err = EINVAL;
				goto parse_out;
			}

			if (fw->dbg_dest_tlv_init)
				break;
			fw->dbg_dest_tlv_init = true;

			dest_v1 = (void *)tlv_data;
			fw->dbg_dest_tlv_v1 = dest_v1;
			fw->n_dest_reg = tlv_len -
			    offsetof(struct iwx_fw_dbg_dest_tlv_v1, reg_ops);
			fw->n_dest_reg /= sizeof(dest_v1->reg_ops[0]);
			DPRINTF(("%s: found debug dest; n_dest_reg=%d\n", __func__, fw->n_dest_reg));
			break;
		}

		case IWX_UCODE_TLV_FW_DBG_CONF: {
			struct iwx_fw_dbg_conf_tlv *conf = (void *)tlv_data;

			if (!fw->dbg_dest_tlv_init ||
			    conf->id >= nitems(fw->dbg_conf_tlv) ||
			    fw->dbg_conf_tlv[conf->id] != NULL)
				break;

			DPRINTF(("Found debug configuration: %d\n", conf->id));
			fw->dbg_conf_tlv[conf->id] = conf;
			fw->dbg_conf_tlv_len[conf->id] = tlv_len;
			break;
		}

		case IWX_UCODE_TLV_UMAC_DEBUG_ADDRS: {
			struct iwx_umac_debug_addrs *dbg_ptrs =
				(void *)tlv_data;

			if (tlv_len != sizeof(*dbg_ptrs)) {
				err = EINVAL;
				goto parse_out;
			}
			if (sc->sc_device_family < IWX_DEVICE_FAMILY_22000)
				break;
			sc->sc_uc.uc_umac_error_event_table =
				le32toh(dbg_ptrs->error_info_addr) &
				~IWX_FW_ADDR_CACHE_CONTROL;
			sc->sc_uc.error_event_table_tlv_status |=
				IWX_ERROR_EVENT_TABLE_UMAC;
			break;
		}

		case IWX_UCODE_TLV_LMAC_DEBUG_ADDRS: {
			struct iwx_lmac_debug_addrs *dbg_ptrs =
				(void *)tlv_data;

			if (tlv_len != sizeof(*dbg_ptrs)) {
				err = EINVAL;
				goto parse_out;
			}
			if (sc->sc_device_family < IWX_DEVICE_FAMILY_22000)
				break;
			sc->sc_uc.uc_lmac_error_event_table[0] =
				le32toh(dbg_ptrs->error_event_table_ptr) &
				~IWX_FW_ADDR_CACHE_CONTROL;
			sc->sc_uc.error_event_table_tlv_status |=
				IWX_ERROR_EVENT_TABLE_LMAC1;
			break;
		}

		case IWX_UCODE_TLV_FW_MEM_SEG:
			break;

		case IWX_UCODE_TLV_IML:
			if (sc->sc_fw.iml != NULL) {
				kmem_free(fw->iml, fw->iml_len);
				fw->iml = NULL;
				fw->iml_len = 0;
			}
			sc->sc_fw.iml = kmem_zalloc(tlv_len, KM_SLEEP);
			if (sc->sc_fw.iml == NULL) {
				err = ENOMEM;
				goto parse_out;
			}
			memcpy(sc->sc_fw.iml, tlv_data, tlv_len);
			sc->sc_fw.iml_len = tlv_len;
			break;

		case IWX_UCODE_TLV_CMD_VERSIONS:
			if (tlv_len % sizeof(struct iwx_fw_cmd_version)) {
				tlv_len /= sizeof(struct iwx_fw_cmd_version);
				tlv_len *= sizeof(struct iwx_fw_cmd_version);
			}
			if (sc->n_cmd_versions != 0) {
				err = EINVAL;
				goto parse_out;
			}
			if (tlv_len > sizeof(sc->cmd_versions)) {
				err = EINVAL;
				goto parse_out;
			}
			memcpy(&sc->cmd_versions[0], tlv_data, tlv_len);
			sc->n_cmd_versions = tlv_len / sizeof(struct iwx_fw_cmd_version);
			break;

		case IWX_UCODE_TLV_FW_RECOVERY_INFO:
			break;

		case IWX_UCODE_TLV_PNVM_DATA:
			if (fw->pnvm != NULL)
				break;
			fw->pnvm = kmem_alloc(tlv_len, KM_SLEEP);
			if (fw->pnvm == NULL) {
				err = ENOMEM;
				goto parse_out;
			}
			memcpy(fw->pnvm, tlv_data, tlv_len);
			fw->pnvm_len = tlv_len;
			break;

		case IWX_UCODE_TLV_FW_FSEQ_VERSION:
		case IWX_UCODE_TLV_PHY_INTEGRATION_VERSION:
		case IWX_UCODE_TLV_FW_NUM_STATIONS:
		case IWX_UCODE_TLV_FW_NUM_BEACONS:
			break;

		/* undocumented TLVs found in iwx-cc-a0-46 image */
		case 58:
		case 0x1000003:
		case 0x1000004:
			break;

		/* undocumented TLVs found in iwx-cc-a0-48 image */
		case 0x1000000:
		case 0x1000002:
			break;

		case IWX_UCODE_TLV_TYPE_DEBUG_INFO:
		case IWX_UCODE_TLV_TYPE_BUFFER_ALLOCATION:
		case IWX_UCODE_TLV_TYPE_HCMD:
		case IWX_UCODE_TLV_TYPE_REGIONS:
		case IWX_UCODE_TLV_TYPE_TRIGGERS:
		case IWX_UCODE_TLV_TYPE_CONF_SET:
		case IWX_UCODE_TLV_SEC_TABLE_ADDR:
		case IWX_UCODE_TLV_D3_KEK_KCK_ADDR:
		case IWX_UCODE_TLV_CURRENT_PC:
			break;

		/* undocumented TLV found in iwx-cc-a0-67 image */
		case 0x100000b:
			break;

		/* undocumented TLV found in iwx-ty-a0-gf-a0-73 image */
		case 0x101:
			break;

		/* undocumented TLV found in iwx-ty-a0-gf-a0-77 image */
		case 0x100000c:
			break;
	
		/* undocumented TLVs found in iwx-bz-a0-gf-a0-92 image */
		case 65:
		case 69:
		case 1092:
			break;

		default:
			err = EINVAL;
			goto parse_out;
		}

		/*
		 * Check for size_t overflow and ignore missing padding at
		 * end of firmware file.
		 */
		if (roundup(tlv_len, 4) > len)
			break;

		len -= roundup(tlv_len, 4);
		data += roundup(tlv_len, 4);
	}

	KASSERT(err == 0);

 parse_out:
	if (err) {
		printf("%s: firmware parse error %d, "
		    "section type %d\n", DEVNAME(sc), err, tlv_type);
	}

 out:
	if (err) {
		fw->fw_status = IWX_FW_STATUS_NONE;
		if (fw->fw_rawdata != NULL)
			iwx_fw_info_free(fw);
	} else
		fw->fw_status = IWX_FW_STATUS_DONE;
	wakeup(&sc->sc_fw);

	return err;
}

static uint32_t
iwx_prph_addr_mask(struct iwx_softc *sc)
{
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		return 0x00ffffff;
	else
		return 0x000fffff;
}

static uint32_t
iwx_read_prph_unlocked(struct iwx_softc *sc, uint32_t addr)
{
	uint32_t mask = iwx_prph_addr_mask(sc);
	IWX_WRITE(sc, IWX_HBUS_TARG_PRPH_RADDR, ((addr & mask) | (3 << 24)));
	IWX_BARRIER_READ_WRITE(sc);
	return IWX_READ(sc, IWX_HBUS_TARG_PRPH_RDAT);
}

static void
iwx_nic_assert_locked(struct iwx_softc *sc)
{
	if (sc->sc_nic_locks <= 0)
		panic("%s: nic locks counter %d", DEVNAME(sc), sc->sc_nic_locks);
}

static uint32_t
iwx_read_prph(struct iwx_softc *sc, uint32_t addr)
{
	iwx_nic_assert_locked(sc);
	return iwx_read_prph_unlocked(sc, addr);
}

static void
iwx_write_prph_unlocked(struct iwx_softc *sc, uint32_t addr, uint32_t val)
{
	uint32_t mask = iwx_prph_addr_mask(sc);
	IWX_WRITE(sc, IWX_HBUS_TARG_PRPH_WADDR, ((addr & mask) | (3 << 24)));
	IWX_BARRIER_WRITE(sc);
	IWX_WRITE(sc, IWX_HBUS_TARG_PRPH_WDAT, val);
}

static void
iwx_write_prph(struct iwx_softc *sc, uint32_t addr, uint32_t val)
{
	iwx_nic_assert_locked(sc);
	iwx_write_prph_unlocked(sc, addr, val);
}

static uint32_t
iwx_read_umac_prph_unlocked(struct iwx_softc *sc, uint32_t addr)
{
	return iwx_read_prph_unlocked(sc, addr + sc->sc_umac_prph_offset);
}

static uint32_t
iwx_read_umac_prph(struct iwx_softc *sc, uint32_t addr)
{
	return iwx_read_prph(sc, addr + sc->sc_umac_prph_offset);
}

static void
iwx_write_umac_prph_unlocked(struct iwx_softc *sc, uint32_t addr, uint32_t val)
{
	iwx_write_prph_unlocked(sc, addr + sc->sc_umac_prph_offset, val);
}

static void
iwx_write_umac_prph(struct iwx_softc *sc, uint32_t addr, uint32_t val)
{
	iwx_write_prph(sc, addr + sc->sc_umac_prph_offset, val);
}

static int
iwx_read_mem(struct iwx_softc *sc, uint32_t addr, void *buf, int dwords)
{
	int offs, err = 0;
	uint32_t *vals = buf;

	if (iwx_nic_lock(sc)) {
		IWX_WRITE(sc, IWX_HBUS_TARG_MEM_RADDR, addr);
		for (offs = 0; offs < dwords; offs++)
			vals[offs] = le32toh(IWX_READ(sc, IWX_HBUS_TARG_MEM_RDAT));
		iwx_nic_unlock(sc);
	} else {
		err = EBUSY;
	}
	return err;
}

static int
iwx_poll_bit(struct iwx_softc *sc, int reg, uint32_t bits, uint32_t mask,
    int timo)
{
	for (;;) {
		if ((IWX_READ(sc, reg) & mask) == (bits & mask)) {
			return 1;
		}
		if (timo < 10) {
			return 0;
		}
		timo -= 10;
		DELAY(10);
	}
}

static int
iwx_nic_lock(struct iwx_softc *sc)
{
	uint32_t access_req, ready, mask;

	if (sc->sc_nic_locks > 0) {
		iwx_nic_assert_locked(sc);
		sc->sc_nic_locks++;
		return 1; /* already locked */
	}

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		access_req = IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ;
		ready = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS;
		mask = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS;
	} else {
		access_req = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ;
		ready = IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN;
		mask = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
		    IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP;
	}

	IWX_SETBITS(sc, IWX_CSR_GP_CNTRL, access_req);

	DELAY(2);

	if (iwx_poll_bit(sc, IWX_CSR_GP_CNTRL, ready, mask, 150000)) {
		sc->sc_nic_locks++;
		return 1;
	}

	printf("%s: acquiring device failed\n", DEVNAME(sc));
	return 0;
}

static void
iwx_nic_unlock(struct iwx_softc *sc)
{
	uint32_t access_req;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		access_req = IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ;
	else
		access_req = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ;

	if (sc->sc_nic_locks > 0) {
		if (--sc->sc_nic_locks == 0)
			IWX_CLRBITS(sc, IWX_CSR_GP_CNTRL, access_req);
	} else
		printf("%s: NIC already unlocked\n", DEVNAME(sc));
}

static int
iwx_set_bits_mask_prph(struct iwx_softc *sc, uint32_t reg, uint32_t bits,
    uint32_t mask)
{
	uint32_t val;

	if (iwx_nic_lock(sc)) {
		val = iwx_read_prph(sc, reg) & mask;
		val |= bits;
		iwx_write_prph(sc, reg, val);
		iwx_nic_unlock(sc);
		return 0;
	}
	return EBUSY;
}

static int
iwx_set_bits_prph(struct iwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return iwx_set_bits_mask_prph(sc, reg, bits, ~0);
}

static int
iwx_clear_bits_prph(struct iwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return iwx_set_bits_mask_prph(sc, reg, 0, ~bits);
}

static int
iwx_dma_contig_alloc(bus_dma_tag_t tag, struct iwx_dma_info *dma,
    bus_size_t size, bus_size_t alignment)
{
	int nsegs, err;
	void *va;

	dma->tag = tag;
	dma->size = size;

	err = bus_dmamap_create(tag, size, 1, size, 0,
	    BUS_DMA_NOWAIT, &dma->map);
	if (err)
		goto fail;

	err = bus_dmamem_alloc(tag, size, alignment, 0, &dma->seg, 1, &nsegs,
	    BUS_DMA_NOWAIT);
	if (err)
		goto fail;

	if (nsegs > 1) {
		err = ENOMEM;
		goto fail;
	}

	err = bus_dmamem_map(tag, &dma->seg, 1, size, &va,
	    BUS_DMA_NOWAIT | BUS_DMA_COHERENT);
	if (err)
		goto fail;
	dma->vaddr = va;

	err = bus_dmamap_load(tag, dma->map, dma->vaddr, size, NULL,
	    BUS_DMA_NOWAIT);
	if (err)
		goto fail;

	memset(dma->vaddr, 0, size);
	bus_dmamap_sync(tag, dma->map, 0, size, BUS_DMASYNC_PREWRITE);
	dma->paddr = dma->map->dm_segs[0].ds_addr;

	return 0;

fail:	iwx_dma_contig_free(dma);
	return err;
}

static void
iwx_dma_contig_free(struct iwx_dma_info *dma)
{
	if (dma->map != NULL) {
		if (dma->vaddr != NULL) {
			bus_dmamap_sync(dma->tag, dma->map, 0, dma->size,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(dma->tag, dma->map);
			bus_dmamem_unmap(dma->tag, dma->vaddr, dma->size);
			bus_dmamem_free(dma->tag, &dma->seg, 1);
			dma->vaddr = NULL;
		}
		bus_dmamap_destroy(dma->tag, dma->map);
		dma->map = NULL;
	}
	dma->size = 0;
}

static int
iwx_alloc_rx_ring(struct iwx_softc *sc, struct iwx_rx_ring *ring)
{
	bus_size_t size;
	int i, err;

	ring->cur = 0;

	/* Allocate RX descriptors (256-byte aligned). */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		size = sizeof(struct iwx_rx_transfer_desc);
	else
		size = sizeof(uint64_t);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->free_desc_dma,
	    size * IWX_RX_MQ_RING_COUNT, 256);
	if (err) {
		printf("%s: could not allocate RX ring DMA memory\n",
		    DEVNAME(sc));
		goto fail;
	}
	ring->desc = ring->free_desc_dma.vaddr;

	/* Allocate RX status area (16-byte aligned). */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		size = sizeof(uint16_t);
	else
		size = sizeof(*ring->stat);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->stat_dma, size, 16);
	if (err) {
		printf("%s: could not allocate RX status DMA memory\n",
		    DEVNAME(sc));
		goto fail;
	}
	ring->stat = ring->stat_dma.vaddr;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		size = sizeof(struct iwx_rx_completion_desc_bz);
	else if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		size = sizeof(struct iwx_rx_completion_desc);
	else
		size = sizeof(uint32_t);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->used_desc_dma,
	    size * IWX_RX_MQ_RING_COUNT, 256);
	if (err) {
		printf("%s: could not allocate RX ring DMA memory\n",
		    DEVNAME(sc));
		goto fail;
	}

	for (i = 0; i < IWX_RX_MQ_RING_COUNT; i++) {
		struct iwx_rx_data *data = &ring->data[i];

		memset(data, 0, sizeof(*data));
		err = bus_dmamap_create(sc->sc_dmat, IWX_RBUF_SIZE, 1,
		    IWX_RBUF_SIZE, 0,
		    BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW,
		    &data->map);
		if (err) {
			printf("%s: could not create RX buf DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}

		err = iwx_rx_addbuf(sc, IWX_RBUF_SIZE, i);
		if (err)
			goto fail;
	}
	return 0;

fail:	iwx_free_rx_ring(sc, ring);
	return err;
}

static void
iwx_disable_rx_dma(struct iwx_softc *sc)
{
	int ntries;

	if (iwx_nic_lock(sc)) {
		if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
			iwx_write_umac_prph(sc, IWX_RFH_RXF_DMA_CFG_GEN3, 0);
		else
			iwx_write_prph(sc, IWX_RFH_RXF_DMA_CFG, 0);
		for (ntries = 0; ntries < 1000; ntries++) {
			if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
				if (iwx_read_umac_prph(sc,
				    IWX_RFH_GEN_STATUS_GEN3) & IWX_RXF_DMA_IDLE)
					break;
			} else {
				if (iwx_read_prph(sc, IWX_RFH_GEN_STATUS) &
				    IWX_RXF_DMA_IDLE)
					break;
			}
			DELAY(10);
		}
		iwx_nic_unlock(sc);
	}
}

static void
iwx_reset_rx_ring(struct iwx_softc *sc, struct iwx_rx_ring *ring)
{
	ring->cur = 0;
	bus_dmamap_sync(sc->sc_dmat, ring->stat_dma.map, 0,
	    ring->stat_dma.size, BUS_DMASYNC_PREWRITE);
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		uint16_t *status = sc->rxq.stat_dma.vaddr;
		*status = 0;
	} else
		memset(ring->stat, 0, sizeof(*ring->stat));
	bus_dmamap_sync(sc->sc_dmat, ring->stat_dma.map, 0,
	    ring->stat_dma.size, BUS_DMASYNC_POSTWRITE);

}

static void
iwx_free_rx_ring(struct iwx_softc *sc, struct iwx_rx_ring *ring)
{
	int i;

	iwx_dma_contig_free(&ring->free_desc_dma);
	iwx_dma_contig_free(&ring->stat_dma);
	iwx_dma_contig_free(&ring->used_desc_dma);

	for (i = 0; i < IWX_RX_MQ_RING_COUNT; i++) {
		struct iwx_rx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(sc->sc_dmat, data->map, 0,
			    data->map->dm_mapsize, BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->sc_dmat, data->map);
			m_freem(data->m);
			data->m = NULL;
		}
		if (data->map != NULL) {
			bus_dmamap_destroy(sc->sc_dmat, data->map);
			data->map = NULL;
		}
	}
}

static int
iwx_alloc_tx_ring(struct iwx_softc *sc, struct iwx_tx_ring *ring, int qid)
{
	bus_addr_t paddr;
	bus_size_t size;
	int i, err;
	size_t bc_tbl_size;
	bus_size_t bc_align;

	ring->qid = qid;
	ring->queued = 0;
	ring->cur = 0;
	ring->cur_hw = 0;
	ring->tail = 0;
	ring->tail_hw = 0;

	/* Allocate TX descriptors (256-byte aligned). */
	size = IWX_TX_RING_COUNT * sizeof(struct iwx_tfh_tfd);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->desc_dma, size, 256);
	if (err) {
		printf("%s: could not allocate TX ring DMA memory\n",
		    DEVNAME(sc));
		goto fail;
	}
	ring->desc = ring->desc_dma.vaddr;

	/*
	 * The hardware supports up to 512 Tx rings which is more
	 * than we currently need.
	 *
	 * In DQA mode we use 1 command queue + 1 default queue for
	 * management, control, and non-QoS data frames.
	 * The command is queue sc->txq[0], our default queue is sc->txq[1].
	 *
	 * Tx aggregation requires additional queues, one queue per TID for
	 * which aggregation is enabled. We map TID 0-7 to sc->txq[2:9].
	 * Firmware may assign its own internal IDs for these queues
	 * depending on which TID gets aggregation enabled first.
	 * The driver maintains a table mapping driver-side queue IDs
	 * to firmware-side queue IDs.
	 */

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		bc_tbl_size = sizeof(struct iwx_gen3_bc_tbl_entry) *
		    IWX_TFD_QUEUE_BC_SIZE_GEN3_AX210;
		bc_align = 128;
	} else {
		bc_tbl_size = sizeof(struct iwx_agn_scd_bc_tbl);
		bc_align = 64;
	}
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->bc_tbl, bc_tbl_size,
	    bc_align);
	if (err) {
		printf("%s: could not allocate byte count table DMA memory\n",
		    DEVNAME(sc));
		goto fail;
	}

	size = IWX_TX_RING_COUNT * sizeof(struct iwx_device_cmd);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &ring->cmd_dma, size,
	    IWX_FIRST_TB_SIZE_ALIGN);
	if (err) {
		printf("%s: could not allocate cmd DMA memory\n", DEVNAME(sc));
		goto fail;
	}
	ring->cmd = ring->cmd_dma.vaddr;

	paddr = ring->cmd_dma.paddr;
	for (i = 0; i < IWX_TX_RING_COUNT; i++) {
		struct iwx_tx_data *data = &ring->data[i];
		size_t mapsize;

		data->cmd_paddr = paddr;
		paddr += sizeof(struct iwx_device_cmd);

		/* FW commands may require more mapped space than packets. */
		if (qid == IWX_DQA_CMD_QUEUE)
			mapsize = (sizeof(struct iwx_cmd_header) +
			    IWX_MAX_CMD_PAYLOAD_SIZE);
		else
			mapsize = MCLBYTES;
		err = bus_dmamap_create(sc->sc_dmat, mapsize,
		    IWX_TFH_NUM_TBS - 2, mapsize, 0,
		    BUS_DMA_NOWAIT, &data->map);
		if (err) {
			printf("%s: could not create TX buf DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}
	}
	KASSERT(paddr == ring->cmd_dma.paddr + size);
	return 0;

fail:	iwx_free_tx_ring(sc, ring);
	return err;
}

static void
iwx_reset_tx_ring(struct iwx_softc *sc, struct iwx_tx_ring *ring)
{
	int i;

	for (i = 0; i < IWX_TX_RING_COUNT; i++) {
		struct iwx_tx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(sc->sc_dmat, data->map, 0,
			    data->map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, data->map);
			m_freem(data->m);
			data->m = NULL;
		}
		if (data->in != NULL) {
			ieee80211_free_node(&data->in->in_ni);
			data->in = NULL;
		}
	}

	/* Clear byte count table. */
	memset(ring->bc_tbl.vaddr, 0, ring->bc_tbl.size);

	/* Clear TX descriptors. */
	memset(ring->desc, 0, ring->desc_dma.size);
	bus_dmamap_sync(sc->sc_dmat, ring->desc_dma.map, 0,
	    ring->desc_dma.size, BUS_DMASYNC_PREWRITE);
	sc->qfullmsk &= ~(1 << ring->qid);
	sc->qenablemsk &= ~(1 << ring->qid);
	for (i = 0; i < nitems(sc->aggqid); i++) {
		if (sc->aggqid[i] == ring->qid) {
			sc->aggqid[i] = 0;
			break;
		}
	}
	ring->queued = 0;
	ring->cur = 0;
	ring->cur_hw = 0;
	ring->tail = 0;
	ring->tail_hw = 0;
	ring->tid = 0;
}

static void
iwx_free_tx_ring(struct iwx_softc *sc, struct iwx_tx_ring *ring)
{
	int i;

	iwx_dma_contig_free(&ring->desc_dma);
	iwx_dma_contig_free(&ring->cmd_dma);
	iwx_dma_contig_free(&ring->bc_tbl);

	for (i = 0; i < IWX_TX_RING_COUNT; i++) {
		struct iwx_tx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(sc->sc_dmat, data->map, 0,
			    data->map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, data->map);
			m_freem(data->m);
			data->m = NULL;
		}
		if (data->map != NULL) {
			bus_dmamap_destroy(sc->sc_dmat, data->map);
			data->map = NULL;
		}
	}
}

static void
iwx_enable_rfkill_int(struct iwx_softc *sc)
{
	if (!sc->sc_msix) {
		sc->sc_intmask = IWX_CSR_INT_BIT_RF_KILL;
		IWX_WRITE(sc, IWX_CSR_INT_MASK, sc->sc_intmask);
	} else {
		IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
		    sc->sc_fh_init_mask);
		IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_MASK_AD,
		    ~IWX_MSIX_HW_INT_CAUSES_REG_RF_KILL);
		sc->sc_hw_mask = IWX_MSIX_HW_INT_CAUSES_REG_RF_KILL;
	}

	IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
	    IWX_CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN);
}

static int
iwx_check_rfkill(struct iwx_softc *sc)
{
	uint32_t v;
	int rv;

	/*
	 * "documentation" is not really helpful here:
	 *  27:	HW_RF_KILL_SW
	 *	Indicates state of (platform's) hardware RF-Kill switch
	 *
	 * But apparently when it's off, it's on ...
	 */
	v = IWX_READ(sc, IWX_CSR_GP_CNTRL);
	rv = (v & IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0;
	if (rv) {
		sc->sc_flags |= IWX_FLAG_RFKILL;
	} else {
		sc->sc_flags &= ~IWX_FLAG_RFKILL;
	}

	return rv;
}

static void
iwx_enable_interrupts(struct iwx_softc *sc)
{
	if (!sc->sc_msix) {
		sc->sc_intmask = IWX_CSR_INI_SET_MASK;
		IWX_WRITE(sc, IWX_CSR_INT_MASK, sc->sc_intmask);
	} else {
		/*
		 * fh/hw_mask keeps all the unmasked causes.
		 * Unlike msi, in msix cause is enabled when it is unset.
		 */
		sc->sc_hw_mask = sc->sc_hw_init_mask;
		sc->sc_fh_mask = sc->sc_fh_init_mask;
		IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
		    ~sc->sc_fh_mask);
		IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_MASK_AD,
		    ~sc->sc_hw_mask);
	}
}

static void
iwx_enable_fwload_interrupt(struct iwx_softc *sc)
{
	if (!sc->sc_msix) {
		sc->sc_intmask = IWX_CSR_INT_BIT_ALIVE | IWX_CSR_INT_BIT_FH_RX;
		IWX_WRITE(sc, IWX_CSR_INT_MASK, sc->sc_intmask);
	} else {
		IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_MASK_AD,
		    ~IWX_MSIX_HW_INT_CAUSES_REG_ALIVE);
		sc->sc_hw_mask = IWX_MSIX_HW_INT_CAUSES_REG_ALIVE;
		/*
		 * Leave all the FH causes enabled to get the ALIVE
		 * notification.
		 */
		IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
		    ~sc->sc_fh_init_mask);
		sc->sc_fh_mask = sc->sc_fh_init_mask;
	}
}

static void
iwx_restore_interrupts(struct iwx_softc *sc)
{
	IWX_WRITE(sc, IWX_CSR_INT_MASK, sc->sc_intmask);
}

static void
iwx_disable_interrupts(struct iwx_softc *sc)
{
	if (!sc->sc_msix) {
		IWX_WRITE(sc, IWX_CSR_INT_MASK, 0);

		/* acknowledge all interrupts */
		IWX_WRITE(sc, IWX_CSR_INT, ~0);
		IWX_WRITE(sc, IWX_CSR_FH_INT_STATUS, ~0);
	} else {
		IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
		    sc->sc_fh_init_mask);
		IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_MASK_AD,
		    sc->sc_hw_init_mask);
		/*
		 * Interrupt processing is deferred to a soft interrupt;
		 * make sure a pending one ignores stale causes.
		 */
		sc->sc_fh_mask = 0;
		sc->sc_hw_mask = 0;
	}
}

static void
iwx_ict_reset(struct iwx_softc *sc)
{
	iwx_disable_interrupts(sc);

	memset(sc->ict_dma.vaddr, 0, IWX_ICT_SIZE);
	sc->ict_cur = 0;

	/* Set physical address of ICT (4KB aligned). */
	IWX_WRITE(sc, IWX_CSR_DRAM_INT_TBL_REG,
	    IWX_CSR_DRAM_INT_TBL_ENABLE
	    | IWX_CSR_DRAM_INIT_TBL_WRAP_CHECK
	    | IWX_CSR_DRAM_INIT_TBL_WRITE_POINTER
	    | sc->ict_dma.paddr >> IWX_ICT_PADDR_SHIFT);

	/* Switch to ICT interrupt mode in driver. */
	sc->sc_flags |= IWX_FLAG_USE_ICT;

	IWX_WRITE(sc, IWX_CSR_INT, ~0);
	iwx_enable_interrupts(sc);
}

#define IWX_HW_READY_TIMEOUT 50
static int
iwx_set_hw_ready(struct iwx_softc *sc)
{
	int ready;

	IWX_SETBITS(sc, IWX_CSR_HW_IF_CONFIG_REG,
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);

	ready = iwx_poll_bit(sc, IWX_CSR_HW_IF_CONFIG_REG,
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
	    IWX_HW_READY_TIMEOUT);
	if (ready)
		IWX_SETBITS(sc, IWX_CSR_MBOX_SET_REG,
		    IWX_CSR_MBOX_SET_REG_OS_ALIVE);

	return ready;
}
#undef IWX_HW_READY_TIMEOUT

static int
iwx_prepare_card_hw(struct iwx_softc *sc)
{
	int t = 0;
	int ntries;

	if (iwx_set_hw_ready(sc))
		return 0;

	IWX_SETBITS(sc, IWX_CSR_DBG_LINK_PWR_MGMT_REG,
	    IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
	DELAY(1000);

	for (ntries = 0; ntries < 10; ntries++) {
		/* If HW is not ready, prepare the conditions to check again */
		IWX_SETBITS(sc, IWX_CSR_HW_IF_CONFIG_REG,
		    IWX_CSR_HW_IF_CONFIG_REG_PREPARE);

		do {
			if (iwx_set_hw_ready(sc))
				return 0;
			DELAY(200);
			t += 200;
		} while (t < 150000);
		DELAY(25000);
	}

	return ETIMEDOUT;
}

static int
iwx_force_power_gating(struct iwx_softc *sc)
{
	int err;

	err = iwx_set_bits_prph(sc, IWX_HPM_HIPM_GEN_CFG,
	    IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
	if (err)
		return err;
	DELAY(20);
	err = iwx_set_bits_prph(sc, IWX_HPM_HIPM_GEN_CFG,
	    IWX_HPM_HIPM_GEN_CFG_CR_PG_EN |
	    IWX_HPM_HIPM_GEN_CFG_CR_SLP_EN);
	if (err)
		return err;
	DELAY(20);
	err = iwx_clear_bits_prph(sc, IWX_HPM_HIPM_GEN_CFG,
	    IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
	return err;
}

static void
iwx_apm_config(struct iwx_softc *sc)
{
	pcireg_t lctl, cap;

	/*
	 * L0S states have been found to be unstable with our devices
	 * and in newer hardware they are not officially supported at
	 * all, so we must always set the L0S_DISABLED bit.
	 */
	IWX_SETBITS(sc, IWX_CSR_GIO_REG, IWX_CSR_GIO_REG_VAL_L0S_DISABLED);

	lctl = pci_conf_read(sc->sc_pct, sc->sc_pcitag,
	    sc->sc_cap_off + PCIE_LCSR);
	sc->sc_pm_support = !(lctl & PCIE_LCSR_ASPM_L0S);
	cap = pci_conf_read(sc->sc_pct, sc->sc_pcitag,
	    sc->sc_cap_off + PCIE_DCSR2);
	sc->sc_ltr_enabled = (cap & PCIE_DCSR2_LTR_MEC) ? 1 : 0;
	DPRINTF(("%s: L1 %sabled - LTR %sabled\n",
	    DEVNAME(sc),
	    (lctl & PCIE_LCSR_ASPM_L1) ? "En" : "Dis",
	    sc->sc_ltr_enabled ? "En" : "Dis"));
}

/*
 * Start up NIC's basic functionality after it has been reset
 * e.g. after platform boot or shutdown.
 * NOTE:  This does not load uCode nor start the embedded processor
 */
static int
iwx_apm_init(struct iwx_softc *sc)
{
	int err = 0;
	uint32_t ready;

	/*
	 * Disable L0s without affecting L1;
	 *  don't wait for ICH L0s (ICH bug W/A)
	 */
	IWX_SETBITS(sc, IWX_CSR_GIO_CHICKEN_BITS,
	    IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);

	/* Set FH wait threshold to maximum (HW error during stress W/A) */
	IWX_SETBITS(sc, IWX_CSR_DBG_HPET_MEM_REG, IWX_CSR_DBG_HPET_MEM_REG_VAL);

	/*
	 * Enable HAP INTA (interrupt from management bus) to
	 * wake device's PCI Express link L1a -> L0s
	 */
	IWX_SETBITS(sc, IWX_CSR_HW_IF_CONFIG_REG,
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);

	iwx_apm_config(sc);

	/*
	 * Set "initialization complete" bit to move adapter from
	 * D0U* --> D0A* (powered-up active) state.
	 */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
		ready = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS;
	} else {
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
		ready = IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY;
	}

	/*
	 * Wait for clock stabilization; once stabilized, access to
	 * device-internal resources is supported, e.g. iwx_write_prph()
	 * and accesses to uCode SRAM.
	 */
	if (!iwx_poll_bit(sc, IWX_CSR_GP_CNTRL, ready, ready, 25000)) {
		printf("%s: timeout waiting for clock stabilization\n",
		    DEVNAME(sc));
		err = ETIMEDOUT;
		goto out;
	}
 out:
	if (err)
		printf("%s: apm init error %d\n", DEVNAME(sc), err);
	return err;
}

static void
iwx_apm_stop(struct iwx_softc *sc)
{
	IWX_SETBITS(sc, IWX_CSR_DBG_LINK_PWR_MGMT_REG,
	    IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
	IWX_SETBITS(sc, IWX_CSR_HW_IF_CONFIG_REG,
	    IWX_CSR_HW_IF_CONFIG_REG_PREPARE |
	    IWX_CSR_HW_IF_CONFIG_REG_ENABLE_PME);
	DELAY(1000);
	IWX_CLRBITS(sc, IWX_CSR_DBG_LINK_PWR_MGMT_REG,
	    IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
	DELAY(5000);

	/* stop device's busmaster DMA activity */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ);

		if (!iwx_poll_bit(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS,
		    IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS, 5000))
			printf("%s: timeout waiting for master\n", DEVNAME(sc));

		DELAY(20000);
	} else {
		IWX_SETBITS(sc, IWX_CSR_RESET,
		    IWX_CSR_RESET_REG_FLAG_STOP_MASTER);

		if (!iwx_poll_bit(sc, IWX_CSR_RESET,
		    IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,
		    IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED, 100))
			printf("%s: timeout waiting for master\n", DEVNAME(sc));
	}

	/*
	 * Clear "initialization complete" bit to move adapter from
	 * D0A* (powered-up Active) --> D0U* (Uninitialized) state.
	 */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_CLRBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
	} else {
		IWX_CLRBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
	}
}

static void
iwx_conf_msix_hw(struct iwx_softc *sc, int stopped)
{
	int vector = 0;

	if (!sc->sc_msix) {
		/* Newer chips default to MSIX. */
		if (!stopped && iwx_nic_lock(sc)) {
			iwx_write_umac_prph(sc, IWX_UREG_CHICK,
			    IWX_UREG_CHICK_MSI_ENABLE);
			iwx_nic_unlock(sc);
		}
		return;
	}

	if (!stopped && iwx_nic_lock(sc)) {
		iwx_write_umac_prph(sc, IWX_UREG_CHICK,
		    IWX_UREG_CHICK_MSIX_ENABLE);
		iwx_nic_unlock(sc);
	}

	/* Disable all interrupts */
	IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_MASK_AD, ~0);
	IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_MASK_AD, ~0);

	/* Map fallback-queue (command/mgmt) to a single vector */
	IWX_WRITE_1(sc, IWX_CSR_MSIX_RX_IVAR(0),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	/* Map RSS queue (data) to the same vector */
	IWX_WRITE_1(sc, IWX_CSR_MSIX_RX_IVAR(1),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);

	/* Enable the RX queues cause interrupts */
	IWX_CLRBITS(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
	    IWX_MSIX_FH_INT_CAUSES_Q0 | IWX_MSIX_FH_INT_CAUSES_Q1);

	/* Map non-RX causes to the same vector */
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_D2S_CH0_NUM),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_D2S_CH1_NUM),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_S2D),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_FH_ERR),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_ALIVE),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_WAKEUP),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_RESET_DONE),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_CT_KILL),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_RF_KILL),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_PERIODIC),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_SW_ERR),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_SW_ERR_V2),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_SCD),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_FH_TX),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_HW_ERR),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);
	IWX_WRITE_1(sc, IWX_CSR_MSIX_IVAR(IWX_MSIX_IVAR_CAUSE_REG_HAP),
	    vector | IWX_MSIX_NON_AUTO_CLEAR_CAUSE);

	/* Enable non-RX causes interrupts */
	IWX_CLRBITS(sc, IWX_CSR_MSIX_FH_INT_MASK_AD,
	    IWX_MSIX_FH_INT_CAUSES_D2S_CH0_NUM |
	    IWX_MSIX_FH_INT_CAUSES_D2S_CH1_NUM |
	    IWX_MSIX_FH_INT_CAUSES_S2D |
	    IWX_MSIX_FH_INT_CAUSES_FH_ERR);
	IWX_CLRBITS(sc, IWX_CSR_MSIX_HW_INT_MASK_AD,
	    IWX_MSIX_HW_INT_CAUSES_REG_ALIVE |
	    IWX_MSIX_HW_INT_CAUSES_REG_WAKEUP |
	    IWX_MSIX_HW_INT_CAUSES_REG_RESET_DONE |
	    IWX_MSIX_HW_INT_CAUSES_REG_CT_KILL |
	    IWX_MSIX_HW_INT_CAUSES_REG_RF_KILL |
	    IWX_MSIX_HW_INT_CAUSES_REG_PERIODIC |
	    IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR |
	    IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_V2 |
	    IWX_MSIX_HW_INT_CAUSES_REG_SCD |
	    IWX_MSIX_HW_INT_CAUSES_REG_FH_TX |
	    IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR |
	    IWX_MSIX_HW_INT_CAUSES_REG_HAP);
}

static void
iwx_init_msix_hw(struct iwx_softc *sc)
{
	iwx_conf_msix_hw(sc, 0);

	if (!sc->sc_msix)
		return;

	sc->sc_fh_init_mask = ~IWX_READ(sc, IWX_CSR_MSIX_FH_INT_MASK_AD);
	sc->sc_fh_mask = sc->sc_fh_init_mask;
	sc->sc_hw_init_mask = ~IWX_READ(sc, IWX_CSR_MSIX_HW_INT_MASK_AD);
	sc->sc_hw_mask = sc->sc_hw_init_mask;
}

static int
iwx_clear_persistence_bit(struct iwx_softc *sc)
{
	uint32_t hpm, wprot;

	hpm = iwx_read_prph_unlocked(sc, IWX_HPM_DEBUG);
	if (hpm != 0xa5a5a5a0 && (hpm & IWX_PERSISTENCE_BIT)) {
		wprot = iwx_read_prph_unlocked(sc, IWX_PREG_PRPH_WPROT_22000);
		if (wprot & IWX_PREG_WFPM_ACCESS) {
			printf("%s: cannot clear persistence bit\n",
			    DEVNAME(sc));
			return EPERM;
		}
		iwx_write_prph_unlocked(sc, IWX_HPM_DEBUG,
		    hpm & ~IWX_PERSISTENCE_BIT);
	}

	return 0;
}

static void
iwx_sw_reset(struct iwx_softc *sc)
{
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET);
		DELAY(20000);
	} else {
		IWX_SETBITS(sc, IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
		DELAY(5000);
	}
}

static int
iwx_start_hw(struct iwx_softc *sc)
{
	int err;

	err = iwx_prepare_card_hw(sc);
	if (err)
		return err;

	if (sc->sc_device_family == IWX_DEVICE_FAMILY_22000) {
		err = iwx_clear_persistence_bit(sc);
		if (err)
			return err;
	}

	/* Reset the entire device */
	iwx_sw_reset(sc);

	if (sc->sc_device_family == IWX_DEVICE_FAMILY_22000 &&
	    sc->sc_integrated) {
		IWX_SETBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
		DELAY(20);
		if (!iwx_poll_bit(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
			printf("%s: timeout waiting for clock stabilization\n",
			    DEVNAME(sc));
			return ETIMEDOUT;
		}

		err = iwx_force_power_gating(sc);
		if (err)
			return err;

		/* Reset the entire device */
		iwx_sw_reset(sc);
	}

	err = iwx_apm_init(sc);
	if (err)
		return err;

	iwx_init_msix_hw(sc);

	iwx_enable_rfkill_int(sc);
	iwx_check_rfkill(sc);

	return 0;
}

static void
iwx_stop_device(struct iwx_softc *sc)
{
	int i;

	iwx_disable_interrupts(sc);
	sc->sc_flags &= ~IWX_FLAG_USE_ICT;

	iwx_disable_rx_dma(sc);
	iwx_reset_rx_ring(sc, &sc->rxq);
	for (i = 0; i < nitems(sc->txq); i++)
		iwx_reset_tx_ring(sc, &sc->txq[i]);

	/* Make sure (redundant) we've released our request to stay awake */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_CLRBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
	} else {
		IWX_CLRBITS(sc, IWX_CSR_GP_CNTRL,
		    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
	}
	if (sc->sc_nic_locks > 0)
		printf("%s: %d active NIC locks forcefully cleared\n",
		    DEVNAME(sc), sc->sc_nic_locks);
	sc->sc_nic_locks = 0;

	/* Stop the device, and put it in low power state */
	iwx_apm_stop(sc);

	/* Reset the on-board processor. */
	iwx_sw_reset(sc);

	/*
	 * Upon stop, the IVAR table gets erased, so msi-x won't
	 * work. This causes a bug in RF-KILL flows, since the interrupt
	 * that enables radio won't fire on the correct irq, and the
	 * driver won't be able to handle the interrupt.
	 * Configure the IVAR table again after reset.
	 */
	iwx_conf_msix_hw(sc, 1);

	/*
	 * Upon stop, the APM issues an interrupt if HW RF kill is set.
	 * Clear the interrupt again.
	 */
	iwx_disable_interrupts(sc);

	/* Even though we stop the HW we still want the RF kill interrupt. */
	iwx_enable_rfkill_int(sc);
	iwx_check_rfkill(sc);

	iwx_prepare_card_hw(sc);

	iwx_ctxt_info_free_paging(sc);
	iwx_dma_contig_free(&sc->pnvm_dma);
	for (i = 0; i < sc->pnvm_segs; i++)
		iwx_dma_contig_free(&sc->pnvm_seg_dma[i]);
	sc->pnvm_segs = 0;
}

static void
iwx_nic_config(struct iwx_softc *sc)
{
	uint8_t radio_cfg_type, radio_cfg_step, radio_cfg_dash;
	uint32_t mask, val, reg_val = 0;

	radio_cfg_type = (sc->sc_fw_phy_config & IWX_FW_PHY_CFG_RADIO_TYPE) >>
	    IWX_FW_PHY_CFG_RADIO_TYPE_POS;
	radio_cfg_step = (sc->sc_fw_phy_config & IWX_FW_PHY_CFG_RADIO_STEP) >>
	    IWX_FW_PHY_CFG_RADIO_STEP_POS;
	radio_cfg_dash = (sc->sc_fw_phy_config & IWX_FW_PHY_CFG_RADIO_DASH) >>
	    IWX_FW_PHY_CFG_RADIO_DASH_POS;

	reg_val |= IWX_CSR_HW_REV_STEP(sc->sc_hw_rev) <<
	    IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP;
	reg_val |= IWX_CSR_HW_REV_DASH(sc->sc_hw_rev) <<
	    IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH;

	/* radio configuration */
	reg_val |= radio_cfg_type << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
	reg_val |= radio_cfg_step << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
	reg_val |= radio_cfg_dash << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;

	mask = IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH |
	    IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP |
	    IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP |
	    IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
	    IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE |
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
	    IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;

	val = IWX_READ(sc, IWX_CSR_HW_IF_CONFIG_REG);
	val &= ~mask;
	val |= reg_val;
	IWX_WRITE(sc, IWX_CSR_HW_IF_CONFIG_REG, val);
}

static int
iwx_nic_rx_init(struct iwx_softc *sc)
{
	IWX_WRITE_1(sc, IWX_CSR_INT_COALESCING, IWX_HOST_INT_TIMEOUT_DEF);

	/*
	 * We don't configure the RFH; the firmware will do that.
	 * Rx descriptors are set when firmware sends an ALIVE interrupt.
	 */
	return 0;
}

static int
iwx_nic_init(struct iwx_softc *sc)
{
	int err;

	iwx_apm_init(sc);
	if (sc->sc_device_family < IWX_DEVICE_FAMILY_AX210)
		iwx_nic_config(sc);

	err = iwx_nic_rx_init(sc);
	if (err)
		return err;

	IWX_SETBITS(sc, IWX_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);

	return 0;
}

/* Map ieee80211 WME access categories to firmware Tx FIFO. */
static const uint8_t iwx_ac_to_tx_fifo[] = {
	IWX_GEN2_EDCA_TX_FIFO_BE,
	IWX_GEN2_EDCA_TX_FIFO_BK,
	IWX_GEN2_EDCA_TX_FIFO_VI,
	IWX_GEN2_EDCA_TX_FIFO_VO,
};

static const uint8_t iwx_ac_to_bz_tx_fifo[] = {
	IWX_BZ_EDCA_TX_FIFO_BE,
	IWX_BZ_EDCA_TX_FIFO_BK,
	IWX_BZ_EDCA_TX_FIFO_VI,
	IWX_BZ_EDCA_TX_FIFO_VO,
};

static int
iwx_enable_txq(struct iwx_softc *sc, int sta_id, int qid, int tid,
    int num_slots)
{
	struct iwx_rx_packet *pkt;
	struct iwx_tx_queue_cfg_rsp *resp;
	struct iwx_tx_queue_cfg_cmd cmd_v0;
	struct iwx_scd_queue_cfg_cmd cmd_v3;
	struct iwx_host_cmd hcmd = {
		.flags = IWX_CMD_WANT_RESP,
		.resp_pkt_len = sizeof(*pkt) + sizeof(*resp),
	};
	struct iwx_tx_ring *ring = &sc->txq[qid];
	int err, fwqid, cmd_ver;
	uint32_t wr_idx;
	size_t resp_len;

	iwx_reset_tx_ring(sc, ring);

	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_SCD_QUEUE_CONFIG_CMD);
	if (cmd_ver == 0 || cmd_ver == IWX_FW_CMD_VER_UNKNOWN) {
		memset(&cmd_v0, 0, sizeof(cmd_v0));
		cmd_v0.sta_id = sta_id;
		cmd_v0.tid = tid;
		cmd_v0.flags = htole16(IWX_TX_QUEUE_CFG_ENABLE_QUEUE);
		cmd_v0.cb_size = htole32(IWX_TFD_QUEUE_CB_SIZE(num_slots));
		cmd_v0.byte_cnt_addr = htole64(ring->bc_tbl.paddr);
		cmd_v0.tfdq_addr = htole64(ring->desc_dma.paddr);
		hcmd.id = IWX_SCD_QUEUE_CFG;
		hcmd.data[0] = &cmd_v0;
		hcmd.len[0] = sizeof(cmd_v0);
	} else if (cmd_ver == 3) {
		memset(&cmd_v3, 0, sizeof(cmd_v3));
		cmd_v3.operation = htole32(IWX_SCD_QUEUE_ADD);
		cmd_v3.u.add.tfdq_dram_addr = htole64(ring->desc_dma.paddr);
		cmd_v3.u.add.bc_dram_addr = htole64(ring->bc_tbl.paddr);
		cmd_v3.u.add.cb_size = htole32(IWX_TFD_QUEUE_CB_SIZE(num_slots));
		cmd_v3.u.add.flags = htole32(0);
		cmd_v3.u.add.sta_mask = htole32(1 << sta_id);
		cmd_v3.u.add.tid = tid;
		hcmd.id = IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_SCD_QUEUE_CONFIG_CMD);
		hcmd.data[0] = &cmd_v3;
		hcmd.len[0] = sizeof(cmd_v3);
	} else {
		printf("%s: unsupported SCD_QUEUE_CFG command version %d\n",
		    DEVNAME(sc), cmd_ver);
		return ENOTSUP;
	}

	err = iwx_send_cmd(sc, &hcmd);
	if (err)
		return err;

	pkt = hcmd.resp_pkt;
	if (!pkt || (pkt->hdr.flags & IWX_CMD_FAILED_MSK)) {
		err = EIO;
		goto out;
	}

	resp_len = iwx_rx_packet_payload_len(pkt);
	if (resp_len != sizeof(*resp)) {
		err = EIO;
		goto out;
	}

	resp = (void *)pkt->data;
	fwqid = le16toh(resp->queue_number);
	wr_idx = le16toh(resp->write_pointer);

	/* Unlike iwlwifi, we do not support dynamic queue ID assignment. */
	if (fwqid != qid) {
		err = EIO;
		goto out;
	}

	if ((int)wr_idx != ring->cur_hw) {
		err = EIO;
		goto out;
	}

	sc->qenablemsk |= (1 << qid);
	ring->tid = tid;
out:
	iwx_free_resp(sc, &hcmd);
	return err;
}

static int
iwx_disable_txq(struct iwx_softc *sc, int sta_id, int qid, uint8_t tid)
{
	struct iwx_rx_packet *pkt;
	struct iwx_tx_queue_cfg_rsp *resp;
	struct iwx_tx_queue_cfg_cmd cmd_v0;
	struct iwx_scd_queue_cfg_cmd cmd_v3;
	struct iwx_host_cmd hcmd = {
		.flags = IWX_CMD_WANT_RESP,
		.resp_pkt_len = sizeof(*pkt) + sizeof(*resp),
	};
	struct iwx_tx_ring *ring = &sc->txq[qid];
	int err, cmd_ver;

	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_SCD_QUEUE_CONFIG_CMD);
	if (cmd_ver == 0 || cmd_ver == IWX_FW_CMD_VER_UNKNOWN) {
		memset(&cmd_v0, 0, sizeof(cmd_v0));
		cmd_v0.sta_id = sta_id;
		cmd_v0.tid = tid;
		cmd_v0.flags = htole16(0); /* clear "queue enabled" flag */
		cmd_v0.cb_size = htole32(0);
		cmd_v0.byte_cnt_addr = htole64(0);
		cmd_v0.tfdq_addr = htole64(0);
		hcmd.id = IWX_SCD_QUEUE_CFG;
		hcmd.data[0] = &cmd_v0;
		hcmd.len[0] = sizeof(cmd_v0);
	} else if (cmd_ver == 3) {
		memset(&cmd_v3, 0, sizeof(cmd_v3));
		cmd_v3.operation = htole32(IWX_SCD_QUEUE_REMOVE);
		cmd_v3.u.remove.sta_mask = htole32(1 << sta_id);
		cmd_v3.u.remove.tid = tid;
		hcmd.id = IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_SCD_QUEUE_CONFIG_CMD);
		hcmd.data[0] = &cmd_v3;
		hcmd.len[0] = sizeof(cmd_v3);
	} else {
		printf("%s: unsupported SCD_QUEUE_CFG command version %d\n",
		    DEVNAME(sc), cmd_ver);
		return ENOTSUP;
	}

	err = iwx_send_cmd(sc, &hcmd);
	if (err)
		return err;

	pkt = hcmd.resp_pkt;
	if (!pkt || (pkt->hdr.flags & IWX_CMD_FAILED_MSK)) {
		err = EIO;
		goto out;
	}

	sc->qenablemsk &= ~(1 << qid);
	iwx_reset_tx_ring(sc, ring);
out:
	iwx_free_resp(sc, &hcmd);
	return err;
}

static void
iwx_post_alive(struct iwx_softc *sc)
{
	int txcmd_ver;

	iwx_ict_reset(sc);

	txcmd_ver = iwx_lookup_notif_ver(sc, IWX_LONG_GROUP, IWX_TX_CMD) ;
	if (txcmd_ver != IWX_FW_CMD_VER_UNKNOWN && txcmd_ver > 6)
		sc->sc_rate_n_flags_version = 2;
	else
		sc->sc_rate_n_flags_version = 1;
}

static int
iwx_schedule_session_protection(struct iwx_softc *sc, struct iwx_node *in,
    uint32_t duration_tu)
{
	struct iwx_session_prot_cmd cmd = {
		.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id,
		    in->in_color)),
		.action = htole32(IWX_FW_CTXT_ACTION_ADD),
		.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC),
		.duration_tu = htole32(duration_tu),
	};
	uint32_t cmd_id;
	int err;

	cmd_id = iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0);
	err = iwx_send_cmd_pdu(sc, cmd_id, 0, sizeof(cmd), &cmd);
	if (!err)
		sc->sc_flags |= IWX_FLAG_TE_ACTIVE;
	return err;
}

static void
iwx_unprotect_session(struct iwx_softc *sc, struct iwx_node *in)
{
	struct iwx_session_prot_cmd cmd = {
		.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id,
		    in->in_color)),
		.action = htole32(IWX_FW_CTXT_ACTION_REMOVE),
		.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC),
		.duration_tu = 0,
	};
	uint32_t cmd_id;

	/* Do nothing if the time event has already ended. */
	if ((sc->sc_flags & IWX_FLAG_TE_ACTIVE) == 0)
		return;

	cmd_id = iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0);
	if (iwx_send_cmd_pdu(sc, cmd_id, 0, sizeof(cmd), &cmd) == 0)
		sc->sc_flags &= ~IWX_FLAG_TE_ACTIVE;
}

/*
 * NVM read access and content parsing.  We do not support
 * external NVM or writing NVM.
 */

static uint8_t
iwx_fw_valid_tx_ant(struct iwx_softc *sc)
{
	uint8_t tx_ant;

	tx_ant = ((sc->sc_fw_phy_config & IWX_FW_PHY_CFG_TX_CHAIN)
	    >> IWX_FW_PHY_CFG_TX_CHAIN_POS);

	if (sc->sc_nvm.valid_tx_ant)
		tx_ant &= sc->sc_nvm.valid_tx_ant;

	return tx_ant;
}

static uint8_t
iwx_fw_valid_rx_ant(struct iwx_softc *sc)
{
	uint8_t rx_ant;

	rx_ant = ((sc->sc_fw_phy_config & IWX_FW_PHY_CFG_RX_CHAIN)
	    >> IWX_FW_PHY_CFG_RX_CHAIN_POS);

	if (sc->sc_nvm.valid_rx_ant)
		rx_ant &= sc->sc_nvm.valid_rx_ant;

	return rx_ant;
}

static void
iwx_init_channel_map(struct iwx_softc *sc, uint16_t *channel_profile_v3,
    uint32_t *channel_profile_v4, int nchan_profile)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_nvm_data *data = &sc->sc_nvm;
	int ch_idx;
	struct ieee80211_channel *channel;
	uint32_t ch_flags;
	int is_5ghz;
	int flags, hw_value;
	int nchan;
	const uint8_t *nvm_channels;

	if (sc->sc_uhb_supported) {
		nchan = nitems(iwx_nvm_channels_uhb);
		nvm_channels = iwx_nvm_channels_uhb;
	} else {
		nchan = nitems(iwx_nvm_channels_8000);
		nvm_channels = iwx_nvm_channels_8000;
	}

	for (ch_idx = 0; ch_idx < nchan && ch_idx < nchan_profile; ch_idx++) {
		if (channel_profile_v4)
			ch_flags = le32_to_cpup(channel_profile_v4 + ch_idx);
		else
			ch_flags = le16_to_cpup(channel_profile_v3 + ch_idx);

		/* net80211 cannot handle 6 GHz channel numbers yet */
		if (ch_idx >= IWX_NUM_2GHZ_CHANNELS + IWX_NUM_5GHZ_CHANNELS)
			break;

		is_5ghz = ch_idx >= IWX_NUM_2GHZ_CHANNELS;
		if (is_5ghz && !data->sku_cap_band_52GHz_enable)
			ch_flags &= ~IWX_NVM_CHANNEL_VALID;

		hw_value = nvm_channels[ch_idx];
		channel = &ic->ic_channels[hw_value];

		if (!(ch_flags & IWX_NVM_CHANNEL_VALID)) {
			channel->ic_freq = 0;
			channel->ic_flags = 0;
			continue;
		}

		if (!is_5ghz) {
			flags = IEEE80211_CHAN_2GHZ;
			channel->ic_flags
			    = IEEE80211_CHAN_CCK
			    | IEEE80211_CHAN_OFDM
			    | IEEE80211_CHAN_DYN
			    | IEEE80211_CHAN_2GHZ;
		} else {
			flags = IEEE80211_CHAN_5GHZ;
			channel->ic_flags =
			    IEEE80211_CHAN_A;
		}
		channel->ic_freq = ieee80211_ieee2mhz(hw_value, flags);

		if (!(ch_flags & IWX_NVM_CHANNEL_ACTIVE))
			channel->ic_flags |= IEEE80211_CHAN_PASSIVE;
	}
}

static void
iwx_set_mac_addr_from_csr(struct iwx_softc *sc, struct iwx_nvm_data *data);
static int
iwx_is_valid_mac_addr(const uint8_t *addr);
static void
iwx_flip_hw_address(uint32_t mac_addr0, uint32_t mac_addr1, uint8_t *dest);

static void
iwx_set_mac_addr_from_csr(struct iwx_softc *sc, struct iwx_nvm_data *data)
{
	uint32_t mac_addr0, mac_addr1;

	memset(data->hw_addr, 0, sizeof(data->hw_addr));

	if (!iwx_nic_lock(sc))
		return;

	mac_addr0 = htole32(IWX_READ(sc, IWX_CSR_MAC_ADDR0_STRAP(sc)));
	mac_addr1 = htole32(IWX_READ(sc, IWX_CSR_MAC_ADDR1_STRAP(sc)));

	iwx_flip_hw_address(mac_addr0, mac_addr1, data->hw_addr);

	/* If OEM fused a valid address, use it instead of the one in OTP. */
	if (iwx_is_valid_mac_addr(data->hw_addr)) {
		iwx_nic_unlock(sc);
		return;
	}

	mac_addr0 = htole32(IWX_READ(sc, IWX_CSR_MAC_ADDR0_OTP(sc)));
	mac_addr1 = htole32(IWX_READ(sc, IWX_CSR_MAC_ADDR1_OTP(sc)));

	iwx_flip_hw_address(mac_addr0, mac_addr1, data->hw_addr);

	iwx_nic_unlock(sc);
}

static int
iwx_is_valid_mac_addr(const uint8_t *addr)
{
	static const uint8_t reserved_mac[] = {
		0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00
	};

	return (memcmp(reserved_mac, addr, ETHER_ADDR_LEN) != 0 &&
	    memcmp(etherbroadcastaddr, addr, sizeof(etherbroadcastaddr)) != 0 &&
	    memcmp(iwx_etheranyaddr, addr, sizeof(iwx_etheranyaddr)) != 0 &&
	    !ETHER_IS_MULTICAST(addr));
}

static void
iwx_flip_hw_address(uint32_t mac_addr0, uint32_t mac_addr1, uint8_t *dest)
{
	const uint8_t *hw_addr;

	hw_addr = (const uint8_t *)&mac_addr0;
	dest[0] = hw_addr[3];
	dest[1] = hw_addr[2];
	dest[2] = hw_addr[1];
	dest[3] = hw_addr[0];

	hw_addr = (const uint8_t *)&mac_addr1;
	dest[4] = hw_addr[1];
	dest[5] = hw_addr[0];
}

static int
iwx_nvm_get(struct iwx_softc *sc)
{
	struct iwx_nvm_get_info cmd = {};
	struct iwx_nvm_data *nvm = &sc->sc_nvm;
	struct iwx_host_cmd hcmd = {
		.flags = IWX_CMD_WANT_RESP | IWX_CMD_SEND_IN_RFKILL,
		.data = { &cmd, },
		.len = { sizeof(cmd) },
		.id = IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,
		    IWX_NVM_GET_INFO)
	};
	int err;
	uint32_t mac_flags;
	/*
	 * All the values in iwx_nvm_get_info_rsp v4 are the same as
	 * in v3, except for the channel profile part of the
	 * regulatory.  So we can just access the new struct, with the
	 * exception of the latter.
	 */
	struct iwx_nvm_get_info_rsp *rsp;
	struct iwx_nvm_get_info_rsp_v3 *rsp_v3;
	int v4 = isset(sc->sc_ucode_api, IWX_UCODE_TLV_API_REGULATORY_NVM_INFO);
	size_t resp_len = v4 ? sizeof(*rsp) : sizeof(*rsp_v3);

	hcmd.resp_pkt_len = sizeof(struct iwx_rx_packet) + resp_len;
	err = iwx_send_cmd(sc, &hcmd);
	if (err)
		return err;

	if (iwx_rx_packet_payload_len(hcmd.resp_pkt) != resp_len) {
		err = EIO;
		goto out;
	}

	memset(nvm, 0, sizeof(*nvm));

	iwx_set_mac_addr_from_csr(sc, nvm);
	if (!iwx_is_valid_mac_addr(nvm->hw_addr)) {
		printf("%s: no valid mac address was found\n", DEVNAME(sc));
		err = EINVAL;
		goto out;
	}

	rsp = (void *)hcmd.resp_pkt->data;

	/* Initialize general data */
	nvm->nvm_version = le16toh(rsp->general.nvm_version);
	nvm->n_hw_addrs = rsp->general.n_hw_addrs;

	/* Initialize MAC sku data */
	mac_flags = le32toh(rsp->mac_sku.mac_sku_flags);
	nvm->sku_cap_11ac_enable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_802_11AC_ENABLED);
	nvm->sku_cap_11n_enable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_802_11N_ENABLED);
	nvm->sku_cap_11ax_enable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_802_11AX_ENABLED);
	nvm->sku_cap_band_24GHz_enable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED);
	nvm->sku_cap_band_52GHz_enable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED);
	nvm->sku_cap_mimo_disable =
		!!(mac_flags & IWX_NVM_MAC_SKU_FLAGS_MIMO_DISABLED);

	/* Initialize PHY sku data */
	nvm->valid_tx_ant = (uint8_t)le32toh(rsp->phy_sku.tx_chains);
	nvm->valid_rx_ant = (uint8_t)le32toh(rsp->phy_sku.rx_chains);

	if (le32toh(rsp->regulatory.lar_enabled) &&
	    isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_LAR_SUPPORT)) {
		nvm->lar_enabled = 1;
	}

	if (v4) {
		iwx_init_channel_map(sc, NULL,
		    rsp->regulatory.channel_profile, IWX_NUM_CHANNELS);
	} else {
		rsp_v3 = (void *)rsp;
		iwx_init_channel_map(sc, rsp_v3->regulatory.channel_profile,
		    NULL, IWX_NUM_CHANNELS_V1);
	}
out:
	iwx_free_resp(sc, &hcmd);
	return err;
}

static int
iwx_load_firmware(struct iwx_softc *sc)
{
	struct iwx_fw_sects *fws;
	int err;

	sc->sc_uc.uc_intr = 0;
	sc->sc_uc.uc_ok = 0;

	fws = &sc->sc_fw.fw_sects[IWX_UCODE_TYPE_REGULAR];
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		err = iwx_ctxt_info_gen3_init(sc, fws);
	else
		err = iwx_ctxt_info_init(sc, fws);
	if (err) {
		printf("%s: could not init context info\n", DEVNAME(sc));
		return err;
	}

	/* wait for the firmware to load */
	err = tsleep(&sc->sc_uc, 0, "iwxuc", SEC_TO_TICKS(1));
	if (err || !sc->sc_uc.uc_ok) {
		printf("%s: could not load firmware, %d\n", DEVNAME(sc), err);
		iwx_ctxt_info_free_paging(sc);
	}

	iwx_dma_contig_free(&sc->iml_dma);
	iwx_ctxt_info_free_fw_img(sc);

	if (!sc->sc_uc.uc_ok)
		return EINVAL;

	return err;
}

static int
iwx_start_fw(struct iwx_softc *sc)
{
	int err;

	IWX_WRITE(sc, IWX_CSR_INT, ~0);

	iwx_disable_interrupts(sc);

	/* make sure rfkill handshake bits are cleared */
	IWX_WRITE(sc, IWX_CSR_UCODE_DRV_GP1_CLR, IWX_CSR_UCODE_SW_BIT_RFKILL);
	IWX_WRITE(sc, IWX_CSR_UCODE_DRV_GP1_CLR,
	    IWX_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);

	/* clear (again), then enable firmware load interrupt */
	IWX_WRITE(sc, IWX_CSR_INT, ~0);

	err = iwx_nic_init(sc);
	if (err) {
		printf("%s: unable to init nic\n", DEVNAME(sc));
		return err;
	}

	iwx_enable_fwload_interrupt(sc);

	return iwx_load_firmware(sc);
}

static int
iwx_pnvm_setup_fragmented(struct iwx_softc *sc, uint8_t **pnvm_data,
    size_t *pnvm_size, int pnvm_segs)
{
	struct iwx_pnvm_info_dram *pnvm_info;
	int i, err;

	err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->pnvm_dma,
	    sizeof(struct iwx_pnvm_info_dram), 0);
	if (err)
		return err;
	pnvm_info = (struct iwx_pnvm_info_dram *)sc->pnvm_dma.vaddr;

	for (i = 0; i < pnvm_segs; i++) {
		err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->pnvm_seg_dma[i],
		    pnvm_size[i], 0);
		if (err)
			goto fail;
		memcpy(sc->pnvm_seg_dma[i].vaddr, pnvm_data[i], pnvm_size[i]);
		pnvm_info->pnvm_img[i] = htole64(sc->pnvm_seg_dma[i].paddr);
		sc->pnvm_size += pnvm_size[i];
		sc->pnvm_segs++;
	}

	return 0;

fail:
	for (i = 0; i < pnvm_segs; i++)
		iwx_dma_contig_free(&sc->pnvm_seg_dma[i]);
	sc->pnvm_size = 0;
	sc->pnvm_segs = 0;
	iwx_dma_contig_free(&sc->pnvm_dma);

	return err;
}

static int
iwx_pnvm_setup(struct iwx_softc *sc, uint8_t **pnvm_data,
    size_t *pnvm_size, int pnvm_segs)
{
	uint8_t *data;
	size_t size = 0;
	int i, err;

	if (isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG))
		return iwx_pnvm_setup_fragmented(sc, pnvm_data, pnvm_size, pnvm_segs);

	for (i = 0; i < pnvm_segs; i++)
		size += pnvm_size[i];

	err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->pnvm_dma, size, 0);
	if (err)
		return err;

	data = sc->pnvm_dma.vaddr;
	for (i = 0; i < pnvm_segs; i++) {
		memcpy(data, pnvm_data[i], pnvm_size[i]);
		data += pnvm_size[i];
	}
	sc->pnvm_size = size;

	return 0;
}

static int
iwx_pnvm_handle_section(struct iwx_softc *sc, const uint8_t *data,
    size_t len)
{
	const struct iwx_ucode_tlv *tlv;
	uint32_t sha1 = 0;
	uint16_t mac_type = 0, rf_id = 0;
	uint8_t *pnvm_data[IWX_MAX_DRAM_ENTRY];
	size_t pnvm_size[IWX_MAX_DRAM_ENTRY];
	int pnvm_segs = 0;
	int hw_match = 0;
	uint32_t size = 0;
	int err;
	int i;

	while (len >= sizeof(*tlv)) {
		uint32_t tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32toh(tlv->length);
		tlv_type = le32toh(tlv->type);

		if (len < tlv_len) {
			printf("%s: invalid TLV len: %zd/%u\n",
			    DEVNAME(sc), len, tlv_len);
			err = EINVAL;
			goto out;
		}

		data += sizeof(*tlv);

		switch (tlv_type) {
		case IWX_UCODE_TLV_PNVM_VERSION:
			if (tlv_len < sizeof(uint32_t))
				break;

			sha1 = le32_to_cpup((const uint32_t *)data);
			break;
		case IWX_UCODE_TLV_HW_TYPE:
			if (tlv_len < 2 * sizeof(uint16_t))
				break;

			if (hw_match)
				break;

			mac_type = le16_to_cpup((const uint16_t *)data);
			rf_id = le16_to_cpup((const uint16_t *)(data +
			    sizeof(uint16_t)));

			if (mac_type == IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) &&
			    rf_id == IWX_CSR_HW_RFID_TYPE(sc->sc_hw_rf_id))
				hw_match = 1;
			break;
		case IWX_UCODE_TLV_SEC_RT: {
			const struct iwx_pnvm_section *section;
			uint32_t data_len;

			section = (const void *)data;
			data_len = tlv_len - sizeof(*section);

			/* TODO: remove, this is a deprecated separator */
			if (le32_to_cpup((const uint32_t *)data) == 0xddddeeee)
				break;

			if (pnvm_segs >= nitems(pnvm_data)) {
				err = ERANGE;
				goto out;
			}

			pnvm_data[pnvm_segs] = kmem_zalloc(data_len, KM_SLEEP);
			if (pnvm_data[pnvm_segs] == NULL) {
				err = ENOMEM;
				goto out;
			}
			memcpy(pnvm_data[pnvm_segs], section->data, data_len);
			pnvm_size[pnvm_segs++] = data_len;
			size += data_len;
			break;
		}
		case IWX_UCODE_TLV_PNVM_SKU:
			/* New PNVM section started, stop parsing. */
			goto done;
		default:
			break;
		}

		if (roundup(tlv_len, 4) > len)
			break;
		len -= roundup(tlv_len, 4);
		data += roundup(tlv_len, 4);
	}
done:
	if (!hw_match || size == 0) {
		err = ENOENT;
		goto out;
	}

	err = iwx_pnvm_setup(sc, pnvm_data, pnvm_size, pnvm_segs);
	if (err) {
		printf("%s: could not allocate DMA memory for PNVM\n",
		    DEVNAME(sc));
		err = ENOMEM;
		goto out;
	}

	iwx_ctxt_info_gen3_set_pnvm(sc);
	sc->sc_pnvm_ver = sha1;
out:
	for (i = 0; i < pnvm_segs; i++)
		kmem_free(pnvm_data[i], pnvm_size[i]);
	return err;
}

static int
iwx_pnvm_parse(struct iwx_softc *sc, const uint8_t *data, size_t len)
{
	const struct iwx_ucode_tlv *tlv;

	while (len >= sizeof(*tlv)) {
		uint32_t tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32toh(tlv->length);
		tlv_type = le32toh(tlv->type);

		if (len < tlv_len || roundup(tlv_len, 4) > len)
			return EINVAL;

		if (tlv_type == IWX_UCODE_TLV_PNVM_SKU) {
			const struct iwx_sku_id *sku_id =
				(const void *)(data + sizeof(*tlv));

			data += sizeof(*tlv) + roundup(tlv_len, 4);
			len -= roundup(tlv_len, 4);

			if (sc->sc_sku_id[0] == le32toh(sku_id->data[0]) &&
			    sc->sc_sku_id[1] == le32toh(sku_id->data[1]) &&
			    sc->sc_sku_id[2] == le32toh(sku_id->data[2]) &&
			    iwx_pnvm_handle_section(sc, data, len) == 0)
				return 0;
		} else {
			data += sizeof(*tlv) + roundup(tlv_len, 4);
			len -= roundup(tlv_len, 4);
		}
	}

	return ENOENT;
}

/* Make AX210 firmware loading context point at PNVM image in DMA memory. */
static void
iwx_ctxt_info_gen3_set_pnvm(struct iwx_softc *sc)
{
	struct iwx_prph_scratch *prph_scratch;
	struct iwx_prph_scratch_ctrl_cfg *prph_sc_ctrl;
	int i;

	prph_scratch = sc->prph_scratch_dma.vaddr;
	prph_sc_ctrl = &prph_scratch->ctrl_cfg;

	prph_sc_ctrl->pnvm_cfg.pnvm_base_addr = htole64(sc->pnvm_dma.paddr);
	prph_sc_ctrl->pnvm_cfg.pnvm_size = htole32(sc->pnvm_size);

	bus_dmamap_sync(sc->sc_dmat, sc->pnvm_dma.map, 0,
	    sc->pnvm_dma.size, BUS_DMASYNC_PREWRITE);
	for (i = 0; i < sc->pnvm_segs; i++)
		bus_dmamap_sync(sc->sc_dmat, sc->pnvm_seg_dma[i].map, 0,
		    sc->pnvm_seg_dma[i].size, BUS_DMASYNC_PREWRITE);
}

/*
 * Load platform-NVM (non-volatile-memory) data from the filesystem.
 * This data apparently contains regulatory information and affects device
 * channel configuration.
 * The SKU of AX210 devices tells us which PNVM file section is needed.
 * Pre-AX210 devices store NVM data onboard.
 */
static int
iwx_load_pnvm(struct iwx_softc *sc)
{
	const int wait_flags = IWX_PNVM_COMPLETE;
	int s, err = 0;
	u_char *pnvm_data = NULL;
	size_t pnvm_size = 0;
	struct iwx_fw_info *fw = &sc->sc_fw;

	if (sc->sc_sku_id[0] == 0 &&
	    sc->sc_sku_id[1] == 0 &&
	    sc->sc_sku_id[2] == 0)
		return 0;

	if (sc->sc_pnvm_name) {
		if (sc->pnvm_dma.vaddr == NULL) {
			/* Prefer PNVM data embedded in firmware image. */
			if (fw->pnvm) {
				err = iwx_pnvm_parse(sc, fw->pnvm,
				    fw->pnvm_len);
				if (err && err != ENOENT)
					return err;
			} else {
				firmware_handle_t fwh;

				err = firmware_open("if_iwx", sc->sc_pnvm_name,
				    &fwh);
				if (err) {
					printf("%s: could not read %s "
					    "(error %d)\n",
					    DEVNAME(sc), sc->sc_pnvm_name,
					    err);
					return err;
				}
				pnvm_size = firmware_get_size(fwh);
				pnvm_data = kmem_alloc(pnvm_size, KM_SLEEP);
				err = firmware_read(fwh, 0, pnvm_data,
				    pnvm_size);
				firmware_close(fwh);
				if (err) {
					printf("%s: could not read %s "
					    "(error %d)\n",
					    DEVNAME(sc), sc->sc_pnvm_name,
					    err);
					kmem_free(pnvm_data, pnvm_size);
					return err;
				}

				err = iwx_pnvm_parse(sc, pnvm_data, pnvm_size);
				if (err && err != ENOENT) {
					kmem_free(pnvm_data, pnvm_size);
					return err;
				}
			}
		} else
			iwx_ctxt_info_gen3_set_pnvm(sc);
	}

	s = splnet();

	if (!iwx_nic_lock(sc)) {
		splx(s);
		if (pnvm_data != NULL)
			kmem_free(pnvm_data, pnvm_size);
		return EBUSY;
	}

	/*
	 * If we don't have a platform NVM file simply ask firmware
	 * to proceed without it.
	 */

	iwx_write_umac_prph(sc, IWX_UREG_DOORBELL_TO_ISR6,
	    IWX_UREG_DOORBELL_TO_ISR6_PNVM);

	/* Wait for the pnvm complete notification from firmware. */
	while ((sc->sc_init_complete & wait_flags) != wait_flags) {
		err = tsleep(&sc->sc_init_complete, 0, "iwxinit",
		    SEC_TO_TICKS(2));
		if (err)
			break;
	}

	splx(s);
	iwx_nic_unlock(sc);
	if (pnvm_data != NULL)
		kmem_free(pnvm_data, pnvm_size);
	return err;
}

static int
iwx_send_tx_ant_cfg(struct iwx_softc *sc, uint8_t valid_tx_ant)
{
	struct iwx_tx_ant_cfg_cmd tx_ant_cmd = {
		.valid = htole32(valid_tx_ant),
	};

	return iwx_send_cmd_pdu(sc, IWX_TX_ANT_CONFIGURATION_CMD,
	    0, sizeof(tx_ant_cmd), &tx_ant_cmd);
}

static int
iwx_send_phy_cfg_cmd(struct iwx_softc *sc)
{
	struct iwx_phy_cfg_cmd phy_cfg_cmd;

	phy_cfg_cmd.phy_cfg = htole32(sc->sc_fw_phy_config);
	phy_cfg_cmd.calib_control.event_trigger =
	    sc->sc_default_calib[IWX_UCODE_TYPE_REGULAR].event_trigger;
	phy_cfg_cmd.calib_control.flow_trigger =
	    sc->sc_default_calib[IWX_UCODE_TYPE_REGULAR].flow_trigger;

	return iwx_send_cmd_pdu(sc, IWX_PHY_CONFIGURATION_CMD, 0,
	    sizeof(phy_cfg_cmd), &phy_cfg_cmd);
}

static int
iwx_send_dqa_cmd(struct iwx_softc *sc)
{
	struct iwx_dqa_enable_cmd dqa_cmd = {
		.cmd_queue = htole32(IWX_DQA_CMD_QUEUE),
	};
	uint32_t cmd_id;

	cmd_id = iwx_cmd_id(IWX_DQA_ENABLE_CMD, IWX_DATA_PATH_GROUP, 0);
	return iwx_send_cmd_pdu(sc, cmd_id, 0, sizeof(dqa_cmd), &dqa_cmd);
}

static int
iwx_load_ucode_wait_alive(struct iwx_softc *sc)
{
	int err;

	err = iwx_read_firmware(sc);
	if (err)
		return err;

	err = iwx_start_fw(sc);
	if (err)
		return err;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		err = iwx_load_pnvm(sc);
		if (err)
			return err;
	}

	iwx_post_alive(sc);

	return 0;
}

static int
iwx_run_init_mvm_ucode(struct iwx_softc *sc, int readnvm)
{
	const int wait_flags = IWX_INIT_COMPLETE;
	struct iwx_nvm_access_complete_cmd nvm_complete = {};
	struct iwx_init_extended_cfg_cmd init_cfg = {
		.init_flags = htole32(IWX_INIT_NVM),
	};
	int err, s;

	if ((sc->sc_flags & IWX_FLAG_RFKILL) && !readnvm) {
		printf("%s: radio is disabled by hardware switch\n",
		    DEVNAME(sc));
		return EPERM;
	}

	s = splnet();
	sc->sc_init_complete = 0;
	err = iwx_load_ucode_wait_alive(sc);
	if (err) {
		printf("%s: failed to load init firmware\n", DEVNAME(sc));
		splx(s);
		return err;
	}

	/*
	 * Send init config command to mark that we are sending NVM
	 * access commands
	 */
	err = iwx_send_cmd_pdu(sc, IWX_WIDE_ID(IWX_SYSTEM_GROUP,
	    IWX_INIT_EXTENDED_CFG_CMD), 0, sizeof(init_cfg), &init_cfg);
	if (err) {
		splx(s);
		return err;
	}

	err = iwx_send_cmd_pdu(sc, IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,
	    IWX_NVM_ACCESS_COMPLETE), 0, sizeof(nvm_complete), &nvm_complete);
	if (err) {
		splx(s);
		return err;
	}

	/* Wait for the init complete notification from the firmware. */
	while ((sc->sc_init_complete & wait_flags) != wait_flags) {
		err = tsleep(&sc->sc_init_complete, 0, "iwxinit",
		    SEC_TO_TICKS(2));
		if (err) {
			splx(s);
			return err;
		}
	}
	splx(s);
	if (readnvm) {
		err = iwx_nvm_get(sc);
		if (err) {
			printf("%s: failed to read nvm\n", DEVNAME(sc));
			return err;
		}
		if (IEEE80211_ADDR_EQ(iwx_etheranyaddr, sc->sc_ic.ic_myaddr))
			IEEE80211_ADDR_COPY(sc->sc_ic.ic_myaddr,
			    sc->sc_nvm.hw_addr);

	}

	/*
	 * Only enable the MLD API on MA/BZ devices for now as the API 77
	 * firmware on some of the older firmware devices also claims
	 * support, but doesn't actually work.
	 */
	if (isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_MLD_API_SUPPORT) &&
	    (IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) == IWX_CFG_MAC_TYPE_MA ||
	    sc->sc_device_family == IWX_DEVICE_FAMILY_BZ))
		sc->sc_use_mld_api = 1;

	return 0;
}

static int
iwx_config_ltr(struct iwx_softc *sc)
{
	struct iwx_ltr_config_cmd cmd = {
		.flags = htole32(IWX_LTR_CFG_FLAG_FEATURE_ENABLE),
	};

	if (!sc->sc_ltr_enabled)
		return 0;

	return iwx_send_cmd_pdu(sc, IWX_LTR_CONFIG, 0, sizeof(cmd), &cmd);
}

static void
iwx_update_rx_desc(struct iwx_softc *sc, struct iwx_rx_ring *ring, int idx)
{
	struct iwx_rx_data *data = &ring->data[idx];

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		struct iwx_rx_transfer_desc *desc = ring->desc;
		desc[idx].rbid = htole16(idx & 0xffff);
		desc[idx].addr = htole64(data->map->dm_segs[0].ds_addr);
		bus_dmamap_sync(sc->sc_dmat, ring->free_desc_dma.map,
		    idx * sizeof(*desc), sizeof(*desc),
		    BUS_DMASYNC_PREWRITE);
	} else {
		((uint64_t *)ring->desc)[idx] =
		    htole64(data->map->dm_segs[0].ds_addr | (idx & 0x0fff));
		bus_dmamap_sync(sc->sc_dmat, ring->free_desc_dma.map,
		    idx * sizeof(uint64_t), sizeof(uint64_t),
		    BUS_DMASYNC_PREWRITE);
	}
}

static int
iwx_rx_addbuf(struct iwx_softc *sc, int size, int idx)
{
	struct iwx_rx_ring *ring = &sc->rxq;
	struct iwx_rx_data *data = &ring->data[idx];
	struct mbuf *m;
	int err;
	int fatal = 0;

	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;

	if (size <= MCLBYTES) {
		MCLGET(m, M_DONTWAIT);
	} else {
		MEXTMALLOC(m, size, M_DONTWAIT);
	}
	if ((m->m_flags & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}

	if (data->m != NULL) {
		bus_dmamap_unload(sc->sc_dmat, data->map);
		fatal = 1;
	}

	m->m_len = m->m_pkthdr.len = size;
	err = bus_dmamap_load_mbuf(sc->sc_dmat, data->map, m,
	    BUS_DMA_READ|BUS_DMA_NOWAIT);
	if (err) {
		/* XXX */
		if (fatal)
			panic("%s: could not load RX mbuf", DEVNAME(sc));
		m_freem(m);
		return err;
	}
	data->m = m;
	bus_dmamap_sync(sc->sc_dmat, data->map, 0, size, BUS_DMASYNC_PREREAD);

	/* Update RX descriptor. */
	iwx_update_rx_desc(sc, ring, idx);

	return 0;
}

static int
iwx_rxmq_get_signal_strength(struct iwx_softc *sc,
    struct iwx_rx_mpdu_desc *desc)
{
	int energy_a, energy_b;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		energy_a = desc->v3.energy_a;
		energy_b = desc->v3.energy_b;
	} else {
		energy_a = desc->v1.energy_a;
		energy_b = desc->v1.energy_b;
	}
	energy_a = energy_a ? -energy_a : -256;
	energy_b = energy_b ? -energy_b : -256;
	return MAX(energy_a, energy_b);
}

static void
iwx_rx_rx_phy_cmd(struct iwx_softc *sc, struct iwx_rx_packet *pkt,
    struct iwx_rx_data *data)
{
	struct iwx_rx_phy_info *phy_info = (void *)pkt->data;

	bus_dmamap_sync(sc->sc_dmat, data->map, sizeof(*pkt),
	    sizeof(*phy_info), BUS_DMASYNC_POSTREAD);

	memcpy(&sc->sc_last_phy_info, phy_info, sizeof(sc->sc_last_phy_info));
}

/*
 * Retrieve the average noise (in dBm) among receivers.
 */
static int
iwx_get_noise(const struct iwx_statistics_rx_non_phy *stats)
{
	int i, total, nbant, noise;

	total = nbant = noise = 0;
	for (i = 0; i < 3; i++) {
		noise = letoh32(stats->beacon_silence_rssi[i]) & 0xff;
		if (noise) {
			total += noise;
			nbant++;
		}
	}

	/* There should be at least one antenna but check anyway. */
	return (nbant == 0) ? -127 : (total / nbant) - 107;
}

/*
 * Pass a received frame to net80211.  rssi_dbm is the signal strength in
 * dBm; net80211 receives a positive value relative to IWX_MIN_DBM.
 */
static void
iwx_rx_frame(struct iwx_softc *sc, struct mbuf *m, int chanidx,
    uint32_t rx_pkt_status, int is_shortpre, int rate_n_flags,
    uint32_t device_timestamp, int rssi_dbm)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct ieee80211_channel *c = NULL;
	int rssi;
	uint8_t type, subtype;

	if (chanidx > 0 && chanidx < nitems(ic->ic_channels) &&
	    ic->ic_channels[chanidx].ic_freq != 0)
		c = &ic->ic_channels[chanidx];
	else
		chanidx = ieee80211_chan2ieee(ic, ic->ic_ibss_chan);

	wh = mtod(m, struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

	/*
	 * net80211 derives the channel of beacons and probe responses
	 * lacking a DS parameter set element from ic_curchan. Make sure
	 * it corresponds to the channel the frame was received on.
	 */
	if (c != NULL && type == IEEE80211_FC0_TYPE_MGT &&
	    (subtype == IEEE80211_FC0_SUBTYPE_BEACON ||
	    subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP))
		ic->ic_curchan = c;

	rssi = rssi_dbm - IWX_MIN_DBM;	/* normalize */
	if (rssi < 0)
		rssi = 0;
	if (rssi > 127)
		rssi = 127;

	m_set_rcvif(m, ifp);

	ni = ieee80211_find_rxnode(ic, (struct ieee80211_frame_min *)wh);
	if (c != NULL && ni != ic->ic_bss)
		ni->ni_chan = c;

	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct iwx_rx_radiotap_header *tap = &sc->sc_rxtap;
		uint16_t chan_flags;
		int have_legacy_rate = 1;
		uint8_t mcs, rate;

		tap->wr_flags = 0;
		if (is_shortpre)
			tap->wr_flags |= IEEE80211_RADIOTAP_F_SHORTPRE;
		tap->wr_chan_freq =
		    htole16(ic->ic_channels[chanidx].ic_freq);
		chan_flags = ic->ic_channels[chanidx].ic_flags;
		tap->wr_chan_flags = htole16(chan_flags);
		tap->wr_dbm_antsignal = (int8_t)rssi_dbm;
		tap->wr_dbm_antnoise = (int8_t)sc->sc_noise;
		tap->wr_tsft = device_timestamp;
		if (sc->sc_rate_n_flags_version >= 2) {
			uint32_t mod_type = (rate_n_flags &
			    IWX_RATE_MCS_MOD_TYPE_MSK);
			const struct ieee80211_rateset *rs = NULL;
			uint32_t ridx;
			have_legacy_rate = (mod_type == IWX_RATE_MCS_CCK_MSK ||
			    mod_type == IWX_RATE_MCS_LEGACY_OFDM_MSK);
			mcs = (rate_n_flags & IWX_RATE_HT_MCS_CODE_MSK);
			ridx = (rate_n_flags & IWX_RATE_LEGACY_RATE_MSK);
			if (mod_type == IWX_RATE_MCS_CCK_MSK)
				rs = &ieee80211_std_rateset_11b;
			else if (mod_type == IWX_RATE_MCS_LEGACY_OFDM_MSK)
				rs = &ieee80211_std_rateset_11a;
			if (rs && ridx < rs->rs_nrates) {
				rate = (rs->rs_rates[ridx] &
				    IEEE80211_RATE_VAL);
			} else
				rate = 0;
		} else {
			have_legacy_rate = ((rate_n_flags &
			    (IWX_RATE_MCS_HT_MSK_V1 |
			    IWX_RATE_MCS_VHT_MSK_V1)) == 0);
			mcs = (rate_n_flags &
			    (IWX_RATE_HT_MCS_RATE_CODE_MSK_V1 |
			    IWX_RATE_HT_MCS_NSS_MSK_V1));
			rate = (rate_n_flags & IWX_RATE_LEGACY_RATE_MSK_V1);
		}
		if (!have_legacy_rate) {
			tap->wr_rate = (0x80 | mcs);
		} else {
			switch (rate) {
			/* CCK rates. */
			case  10: tap->wr_rate =   2; break;
			case  20: tap->wr_rate =   4; break;
			case  55: tap->wr_rate =  11; break;
			case 110: tap->wr_rate =  22; break;
			/* OFDM rates. */
			case 0xd: tap->wr_rate =  12; break;
			case 0xf: tap->wr_rate =  18; break;
			case 0x5: tap->wr_rate =  24; break;
			case 0x7: tap->wr_rate =  36; break;
			case 0x9: tap->wr_rate =  48; break;
			case 0xb: tap->wr_rate =  72; break;
			case 0x1: tap->wr_rate =  96; break;
			case 0x3: tap->wr_rate = 108; break;
			/* Unknown rate: should not happen. */
			default:  tap->wr_rate =   0;
			}
		}

		bpf_mtap2(sc->sc_drvbpf, tap, sc->sc_rxtap_len, m, BPF_D_IN);
	}

	ieee80211_input(ic, m, ni, rssi, device_timestamp);
	ieee80211_free_node(ni);
}

static void
iwx_rx_mpdu_mq(struct iwx_softc *sc, struct mbuf *m, void *pktdata,
    size_t maxlen)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	struct iwx_rx_mpdu_desc *desc;
	uint32_t len, hdrlen, rate_n_flags, device_timestamp;
	int rssi;
	uint8_t chanidx;
	uint16_t phy_info;
	size_t desc_size;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		desc_size = sizeof(*desc);
	else
		desc_size = IWX_RX_DESC_SIZE_V1;

	if (maxlen < desc_size) {
		m_freem(m);
		return; /* drop */
	}

	desc = (struct iwx_rx_mpdu_desc *)pktdata;

	if (!(desc->status & htole32(IWX_RX_MPDU_RES_STATUS_CRC_OK)) ||
	    !(desc->status & htole32(IWX_RX_MPDU_RES_STATUS_OVERRUN_OK))) {
		m_freem(m);
		return; /* drop */
	}

	len = le16toh(desc->mpdu_len);
	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		/* Allow control frames in monitor mode. */
		if (len < sizeof(struct ieee80211_frame_cts)) {
			ic->ic_stats.is_rx_tooshort++;
			if_statinc(ifp, if_ierrors);
			m_freem(m);
			return;
		}
	} else if (len < sizeof(struct ieee80211_frame)) {
		ic->ic_stats.is_rx_tooshort++;
		if_statinc(ifp, if_ierrors);
		m_freem(m);
		return;
	}
	if (len > maxlen - desc_size) {
		if_statinc(ifp, if_ierrors);
		m_freem(m);
		return;
	}

	m->m_data = (char *)pktdata + desc_size;
	m->m_pkthdr.len = m->m_len = len;

	/* Account for padding following the frame header. */
	if (desc->mac_flags2 & IWX_RX_MPDU_MFLG2_PAD) {
		struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
		int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		if (type == IEEE80211_FC0_TYPE_CTL) {
			switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
			case IEEE80211_FC0_SUBTYPE_CTS:
				hdrlen = sizeof(struct ieee80211_frame_cts);
				break;
			case IEEE80211_FC0_SUBTYPE_ACK:
				hdrlen = sizeof(struct ieee80211_frame_ack);
				break;
			default:
				hdrlen = sizeof(struct ieee80211_frame_min);
				break;
			}
		} else
			hdrlen = ieee80211_anyhdrsize(wh);

		if ((le32toh(desc->status) &
		    IWX_RX_MPDU_RES_STATUS_SEC_ENC_MSK) ==
		    IWX_RX_MPDU_RES_STATUS_SEC_CCM_ENC) {
			/* Padding is inserted after the IV. */
			hdrlen += IEEE80211_CCMP_HDRLEN;
		}

		if (hdrlen + 2 > len) {
			if_statinc(ifp, if_ierrors);
			m_freem(m);
			return;
		}
		memmove(m->m_data + 2, m->m_data, hdrlen);
		m_adj(m, 2);
	}

	/*
	 * We do not install keys into the firmware, so all frames should
	 * arrive undecrypted and net80211's software crypto takes care
	 * of them. If the firmware claims it decrypted a frame with a
	 * bad MIC there is nothing useful we can do with it.
	 */
	if ((le32toh(desc->status) & IWX_RX_MPDU_RES_STATUS_SEC_ENC_MSK) !=
	    IWX_RX_MPDU_RES_STATUS_SEC_NO_ENC &&
	    (le32toh(desc->status) & IWX_RX_MPDU_RES_STATUS_DEC_DONE) &&
	    !(le32toh(desc->status) & IWX_RX_MPDU_RES_STATUS_MIC_OK)) {
		if_statinc(ifp, if_ierrors);
		m_freem(m);
		return;
	}

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		rate_n_flags = le32toh(desc->v3.rate_n_flags);
		chanidx = desc->v3.channel;
		device_timestamp = le32toh(desc->v3.gp2_on_air_rise);
	} else {
		rate_n_flags = le32toh(desc->v1.rate_n_flags);
		chanidx = desc->v1.channel;
		device_timestamp = le32toh(desc->v1.gp2_on_air_rise);
	}

	phy_info = le16toh(desc->phy_info);

	rssi = iwx_rxmq_get_signal_strength(sc, desc);

	iwx_rx_frame(sc, m, chanidx, le32toh(desc->status),
	    (phy_info & IWX_RX_MPDU_PHY_SHORT_PREAMBLE),
	    rate_n_flags, device_timestamp, rssi);
}

static void
iwx_clear_tx_desc(struct iwx_softc *sc, struct iwx_tx_ring *ring, int idx)
{
	struct iwx_tfh_tfd *desc = &ring->desc[idx];
	uint8_t num_tbs = le16toh(desc->num_tbs) & 0x1f;
	int i;

	/* First TB is never cleared - it is bidirectional DMA data. */
	for (i = 1; i < num_tbs; i++) {
		struct iwx_tfh_tb *tb = &desc->tbs[i];
		memset(tb, 0, sizeof(*tb));
	}
	desc->num_tbs = htole16(1);

	bus_dmamap_sync(sc->sc_dmat, ring->desc_dma.map,
	    (char *)(void *)desc - (char *)(void *)ring->desc_dma.vaddr,
	    sizeof(*desc), BUS_DMASYNC_PREWRITE);
}

static void
iwx_txd_done(struct iwx_softc *sc, struct iwx_tx_data *txd)
{
	bus_dmamap_sync(sc->sc_dmat, txd->map, 0, txd->map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->sc_dmat, txd->map);
	m_freem(txd->m);
	txd->m = NULL;

	KASSERT(txd->in);
	ieee80211_free_node(&txd->in->in_ni);
	txd->in = NULL;
}

static void
iwx_txq_advance(struct iwx_softc *sc, struct iwx_tx_ring *ring, uint16_t idx)
{
 	struct iwx_tx_data *txd;

	while (ring->tail_hw != idx) {
		txd = &ring->data[ring->tail];
		if (txd->m != NULL) {
			iwx_clear_tx_desc(sc, ring, ring->tail);
			iwx_tx_update_byte_tbl(sc, ring, ring->tail, 0, 0);
			iwx_txd_done(sc, txd);
			ring->queued--;
		}
		ring->tail = (ring->tail + 1) % IWX_TX_RING_COUNT;
		ring->tail_hw = (ring->tail_hw + 1) % sc->max_tfd_queue_size;
	}
}

static void
iwx_clear_oactive(struct iwx_softc *sc, struct iwx_tx_ring *ring)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);

	if (ring->queued < IWX_TX_RING_LOMARK) {
		sc->qfullmsk &= ~(1 << ring->qid);
		if (sc->qfullmsk == 0 && (ifp->if_flags & IFF_OACTIVE)) {
			ifp->if_flags &= ~IFF_OACTIVE;
			/*
			 * Well, we're in interrupt context, but then again
			 * I guess net80211 does all sorts of stunts in
			 * interrupt context, so maybe this is no biggie.
			 */
			iwx_start(ifp);
		}
	}
}

static void
iwx_rx_tx_cmd(struct iwx_softc *sc, struct iwx_rx_packet *pkt,
    struct iwx_rx_data *data)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	struct iwx_cmd_header *cmd_hdr = &pkt->hdr;
	int qid = cmd_hdr->qid, status, txfail;
	struct iwx_tx_ring *ring;
	struct iwx_tx_resp *tx_resp = (void *)pkt->data;
	uint32_t ssn;
	uint32_t len = iwx_rx_packet_len(pkt);

	if (qid >= nitems(sc->txq))
		return;
	ring = &sc->txq[qid];

	bus_dmamap_sync(sc->sc_dmat, data->map, 0, IWX_RBUF_SIZE,
	    BUS_DMASYNC_POSTREAD);

	/* Sanity checks. */
	if (sizeof(*tx_resp) > len)
		return;
	if (qid < IWX_FIRST_AGG_TX_QUEUE && tx_resp->frame_count > 1)
		return;
	if (qid >= IWX_FIRST_AGG_TX_QUEUE && sizeof(*tx_resp) + sizeof(ssn) +
	    tx_resp->frame_count * sizeof(tx_resp->status) > len)
		return;

	sc->sc_tx_timer[qid] = 0;

	if (tx_resp->frame_count > 1) /* A-MPDU */
		return;

	status = le16toh(tx_resp->status.status) & IWX_TX_STATUS_MSK;
	txfail = (status != IWX_TX_STATUS_SUCCESS &&
	    status != IWX_TX_STATUS_DIRECT_DONE);

	if (txfail)
		if_statinc(ifp, if_oerrors);
	else
		if_statinc(ifp, if_opackets);

	/*
	 * On hardware supported by iwx(4) the SSN counter corresponds
	 * to a Tx ring index rather than a sequence number.
	 * Frames up to this index (non-inclusive) can now be freed.
	 */
	memcpy(&ssn, &tx_resp->status + tx_resp->frame_count, sizeof(ssn));
	ssn = le32toh(ssn);
	if (ssn < (uint32_t)sc->max_tfd_queue_size) {
		iwx_txq_advance(sc, ring, ssn);
		iwx_clear_oactive(sc, ring);
	}
}

static void
iwx_rx_bmiss(struct iwx_softc *sc, struct iwx_rx_packet *pkt,
    struct iwx_rx_data *data)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_missed_beacons_notif *mbn = (void *)pkt->data;
	uint32_t missed;

	if ((ic->ic_opmode != IEEE80211_M_STA) ||
	    (ic->ic_state != IEEE80211_S_RUN))
		return;

	bus_dmamap_sync(sc->sc_dmat, data->map, sizeof(*pkt),
	    sizeof(*mbn), BUS_DMASYNC_POSTREAD);

	missed = le32toh(mbn->consec_missed_beacons_since_last_rx);
	if ((int)missed > ic->ic_bmiss_max && ic->ic_mgt_timer == 0) {
		if (IC2IFP(ic)->if_flags & IFF_DEBUG)
			printf("%s: receiving no beacons from %s; checking if "
			    "this AP is still responding to probe requests\n",
			    DEVNAME(sc), ether_sprintf(ic->ic_bss->ni_macaddr));
		/*
		 * Let net80211 send a directed probe request first and
		 * fall back to scanning if that fails repeatedly.
		 */
		ieee80211_beacon_miss(ic);
	}
}

static int
iwx_binding_cmd(struct iwx_softc *sc, struct iwx_node *in, uint32_t action)
{
	struct iwx_binding_cmd cmd;
	struct iwx_phy_ctxt *phyctxt = in->in_phyctxt;
	uint32_t mac_id = IWX_FW_CMD_ID_AND_COLOR(in->in_id, in->in_color);
	int i, err, active = (sc->sc_flags & IWX_FLAG_BINDING_ACTIVE);
	uint32_t status;

	/* No need to bind with MLD firmware. */
	if (sc->sc_use_mld_api)
		return 0;
	
	if (action == IWX_FW_CTXT_ACTION_ADD && active)
		panic("binding already added");
	if (action == IWX_FW_CTXT_ACTION_REMOVE && !active)
		panic("binding already removed");

	/*
	 * ic_bss may have been replaced by net80211 since the binding
	 * was added; we only ever use the first PHY context anyway.
	 */
	if (phyctxt == NULL)
		phyctxt = &sc->sc_phyctxt[0];

	memset(&cmd, 0, sizeof(cmd));

	cmd.id_and_color
	    = htole32(IWX_FW_CMD_ID_AND_COLOR(phyctxt->id, phyctxt->color));
	cmd.action = htole32(action);
	cmd.phy = htole32(IWX_FW_CMD_ID_AND_COLOR(phyctxt->id, phyctxt->color));

	cmd.macs[0] = htole32(mac_id);
	for (i = 1; i < IWX_MAX_MACS_IN_BINDING; i++)
		cmd.macs[i] = htole32(IWX_FW_CTXT_INVALID);

	if (phyctxt->channel == NULL ||
	    IEEE80211_IS_CHAN_2GHZ(phyctxt->channel) ||
	    !isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
		cmd.lmac_id = htole32(IWX_LMAC_24G_INDEX);
	else
		cmd.lmac_id = htole32(IWX_LMAC_5G_INDEX);

	status = 0;
	err = iwx_send_cmd_pdu_status(sc, IWX_BINDING_CONTEXT_CMD, sizeof(cmd),
	    &cmd, &status);
	if (err == 0 && status != 0)
		err = EIO;

	return err;
}

static int
iwx_phy_ctxt_cmd_uhb_v3_v4(struct iwx_softc *sc, struct iwx_phy_ctxt *ctxt,
    uint8_t chains_static, uint8_t chains_dynamic, uint32_t action, uint8_t sco,
    uint8_t vht_chan_width, int cmdver)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_phy_context_cmd_uhb cmd;
	uint8_t active_cnt, idle_cnt;
	struct ieee80211_channel *chan = ctxt->channel;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(ctxt->id,
	    ctxt->color));
	cmd.action = htole32(action);

	if (IEEE80211_IS_CHAN_2GHZ(ctxt->channel) ||
	    !isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
		cmd.lmac_id = htole32(IWX_LMAC_24G_INDEX);
	else
		cmd.lmac_id = htole32(IWX_LMAC_5G_INDEX);

	cmd.ci.band = IEEE80211_IS_CHAN_2GHZ(chan) ?
	    IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
	cmd.ci.channel = htole32(ieee80211_chan2ieee(ic, chan));
	cmd.ci.width = IWX_PHY_VHT_CHANNEL_MODE20;
	cmd.ci.ctrl_pos = IWX_PHY_VHT_CTRL_POS_1_BELOW;

	if (iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_RLC_CONFIG_CMD) != 2) {
		idle_cnt = chains_static;
		active_cnt = chains_dynamic;
		cmd.rxchain_info = htole32(iwx_fw_valid_rx_ant(sc) <<
		    IWX_PHY_RX_CHAIN_VALID_POS);
		cmd.rxchain_info |= htole32(idle_cnt <<
		    IWX_PHY_RX_CHAIN_CNT_POS);
		cmd.rxchain_info |= htole32(active_cnt <<
		    IWX_PHY_RX_CHAIN_MIMO_CNT_POS);
	}

	return iwx_send_cmd_pdu(sc, IWX_PHY_CONTEXT_CMD, 0, sizeof(cmd), &cmd);
}

static int
iwx_phy_ctxt_cmd_v3_v4(struct iwx_softc *sc, struct iwx_phy_ctxt *ctxt,
    uint8_t chains_static, uint8_t chains_dynamic, uint32_t action, uint8_t sco,
    uint8_t vht_chan_width, int cmdver)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_phy_context_cmd cmd;
	uint8_t active_cnt, idle_cnt;
	struct ieee80211_channel *chan = ctxt->channel;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(ctxt->id,
	    ctxt->color));
	cmd.action = htole32(action);

	if (IEEE80211_IS_CHAN_2GHZ(ctxt->channel) ||
	    !isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
		cmd.lmac_id = htole32(IWX_LMAC_24G_INDEX);
	else
		cmd.lmac_id = htole32(IWX_LMAC_5G_INDEX);

	cmd.ci.band = IEEE80211_IS_CHAN_2GHZ(chan) ?
	    IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
	cmd.ci.channel = ieee80211_chan2ieee(ic, chan);
	cmd.ci.width = IWX_PHY_VHT_CHANNEL_MODE20;
	cmd.ci.ctrl_pos = IWX_PHY_VHT_CTRL_POS_1_BELOW;

	if (iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_RLC_CONFIG_CMD) != 2) {
		idle_cnt = chains_static;
		active_cnt = chains_dynamic;
		cmd.rxchain_info = htole32(iwx_fw_valid_rx_ant(sc) <<
		    IWX_PHY_RX_CHAIN_VALID_POS);
		cmd.rxchain_info |= htole32(idle_cnt <<
		    IWX_PHY_RX_CHAIN_CNT_POS);
		cmd.rxchain_info |= htole32(active_cnt <<
		    IWX_PHY_RX_CHAIN_MIMO_CNT_POS);
	}

	return iwx_send_cmd_pdu(sc, IWX_PHY_CONTEXT_CMD, 0, sizeof(cmd), &cmd);
}

static int
iwx_phy_ctxt_cmd(struct iwx_softc *sc, struct iwx_phy_ctxt *ctxt,
    uint8_t chains_static, uint8_t chains_dynamic, uint32_t action,
    uint32_t apply_time, uint8_t sco, uint8_t vht_chan_width)
{
	int cmdver;

	cmdver = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD);
	if (cmdver != 3 && cmdver != 4) {
		printf("%s: firmware does not support phy-context-cmd v3/v4\n",
		    DEVNAME(sc));
		return ENOTSUP;
	}

	/*
	 * Intel increased the size of the fw_channel_info struct and neglected
	 * to bump the phy_context_cmd struct, which contains an fw_channel_info
	 * member in the middle.
	 * To keep things simple we use a separate function to handle the larger
	 * variant of the phy context command.
	 */
	if (isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS)) {
		return iwx_phy_ctxt_cmd_uhb_v3_v4(sc, ctxt, chains_static,
		    chains_dynamic, action, sco, vht_chan_width, cmdver);
	}

	return iwx_phy_ctxt_cmd_v3_v4(sc, ctxt, chains_static, chains_dynamic,
	    action, sco, vht_chan_width, cmdver);
}

static int
iwx_send_cmd(struct iwx_softc *sc, struct iwx_host_cmd *hcmd)
{
	struct iwx_tx_ring *ring = &sc->txq[IWX_DQA_CMD_QUEUE];
	struct iwx_tfh_tfd *desc;
	struct iwx_tx_data *txdata;
	struct iwx_device_cmd *cmd;
	struct mbuf *m;
	bus_addr_t paddr;
	uint64_t addr;
	int err = 0, i, s;
	size_t paylen, off;
	int idx, code, async, group_id;
	size_t hdrlen, datasz;
	uint8_t *data;
	int generation = sc->sc_generation;

	code = hcmd->id;
	async = hcmd->flags & IWX_CMD_ASYNC;
	idx = ring->cur;

	for (i = 0, paylen = 0; i < nitems(hcmd->len); i++) {
		paylen += hcmd->len[i];
	}

	/* If this command waits for a response, allocate response buffer. */
	hcmd->resp_pkt = NULL;
	if (hcmd->flags & IWX_CMD_WANT_RESP) {
		uint8_t *resp_buf;
		KASSERT(!async);
		KASSERT(hcmd->resp_pkt_len >= sizeof(struct iwx_rx_packet));
		KASSERT(hcmd->resp_pkt_len <= IWX_CMD_RESP_MAX);
		if (sc->sc_cmd_resp_pkt[idx] != NULL)
			return ENOSPC;
		resp_buf = kmem_intr_zalloc(hcmd->resp_pkt_len, KM_NOSLEEP);
		if (resp_buf == NULL)
			return ENOMEM;
		sc->sc_cmd_resp_pkt[idx] = resp_buf;
		sc->sc_cmd_resp_len[idx] = hcmd->resp_pkt_len;
	} else {
		sc->sc_cmd_resp_pkt[idx] = NULL;
	}

	s = splnet();

	desc = &ring->desc[idx];
	txdata = &ring->data[idx];

	/*
	 * XXX Intel inside (tm)
	 * Firmware API versions >= 50 reject old-style commands in
	 * group 0 with a "BAD_COMMAND" firmware error. We must pretend
	 * that such commands were in the LONG_GROUP instead in order
	 * for firmware to accept them.
	 */
	if (iwx_cmd_groupid(code) == 0) {
		code = IWX_WIDE_ID(IWX_LONG_GROUP, code);
		txdata->flags |= IWX_TXDATA_FLAG_CMD_IS_NARROW;
	} else
		txdata->flags &= ~IWX_TXDATA_FLAG_CMD_IS_NARROW;

	group_id = iwx_cmd_groupid(code);

	hdrlen = sizeof(cmd->hdr_wide);
	datasz = sizeof(cmd->data_wide);

	if (paylen > datasz) {
		/* Command is too large to fit in pre-allocated space. */
		size_t totlen = hdrlen + paylen;
		if (paylen > IWX_MAX_CMD_PAYLOAD_SIZE) {
			printf("%s: firmware command too long (%zd bytes)\n",
			    DEVNAME(sc), totlen);
			err = EINVAL;
			goto out;
		}
		m = m_gethdr(M_DONTWAIT, MT_DATA);
		if (m == NULL) {
			err = ENOMEM;
			goto out;
		}
		MEXTMALLOC(m, totlen, M_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			printf("%s: could not get fw cmd mbuf (%zd bytes)\n",
			    DEVNAME(sc), totlen);
			m_freem(m);
			err = ENOMEM;
			goto out;
		}
		cmd = mtod(m, struct iwx_device_cmd *);
		err = bus_dmamap_load(sc->sc_dmat, txdata->map, cmd,
		    totlen, NULL, BUS_DMA_NOWAIT | BUS_DMA_WRITE);
		if (err) {
			printf("%s: could not load fw cmd mbuf (%zd bytes)\n",
			    DEVNAME(sc), totlen);
			m_freem(m);
			goto out;
		}
		txdata->m = m; /* mbuf will be freed in iwx_cmd_done() */
		paddr = txdata->map->dm_segs[0].ds_addr;
	} else {
		cmd = &ring->cmd[idx];
		paddr = txdata->cmd_paddr;
	}

	memset(cmd, 0, sizeof(*cmd));
	cmd->hdr_wide.opcode = iwx_cmd_opcode(code);
	cmd->hdr_wide.group_id = group_id;
	cmd->hdr_wide.qid = ring->qid;
	cmd->hdr_wide.idx = idx;
	cmd->hdr_wide.length = htole16(paylen);
	cmd->hdr_wide.version = iwx_cmd_version(code);
	data = cmd->data_wide;

	for (i = 0, off = 0; i < nitems(hcmd->data); i++) {
		if (hcmd->len[i] == 0)
			continue;
		memcpy(data + off, hcmd->data[i], hcmd->len[i]);
		off += hcmd->len[i];
	}
	KASSERT(off == paylen);

	desc->tbs[0].tb_len = htole16(MIN(hdrlen + paylen, IWX_FIRST_TB_SIZE));
	addr = htole64(paddr);
	memcpy(&desc->tbs[0].addr, &addr, sizeof(addr));
	if (hdrlen + paylen > IWX_FIRST_TB_SIZE) {
		desc->tbs[1].tb_len = htole16(hdrlen + paylen -
		    IWX_FIRST_TB_SIZE);
		addr = htole64(paddr + IWX_FIRST_TB_SIZE);
		memcpy(&desc->tbs[1].addr, &addr, sizeof(addr));
		desc->num_tbs = htole16(2);
	} else
		desc->num_tbs = htole16(1);

	if (paylen > datasz) {
		bus_dmamap_sync(sc->sc_dmat, txdata->map, 0,
		    hdrlen + paylen, BUS_DMASYNC_PREWRITE);
	} else {
		bus_dmamap_sync(sc->sc_dmat, ring->cmd_dma.map,
		    (char *)(void *)cmd - (char *)(void *)ring->cmd_dma.vaddr,
		    hdrlen + paylen, BUS_DMASYNC_PREWRITE);
	}
	bus_dmamap_sync(sc->sc_dmat, ring->desc_dma.map,
	    (char *)(void *)desc - (char *)(void *)ring->desc_dma.vaddr,
	    sizeof (*desc), BUS_DMASYNC_PREWRITE);
	/* Kick command ring. */
	DPRINTFN(3, ("%s: sending command 0x%x\n", __func__, code));
	ring->queued++;
	ring->cur = (ring->cur + 1) % IWX_TX_RING_COUNT;
	ring->cur_hw = (ring->cur_hw + 1) % sc->max_tfd_queue_size;
	IWX_WRITE(sc, IWX_HBUS_TARG_WRPTR, ring->qid << 16 | ring->cur_hw);

	if (!async) {
		err = tsleep(desc, PCATCH, "iwxcmd", SEC_TO_TICKS(1));
		if (err == 0) {
			/* if hardware is no longer up, return error */
			if (generation != sc->sc_generation) {
				err = ENXIO;
				goto out;
			}

			/* Response buffer will be freed in iwx_free_resp(). */
			hcmd->resp_pkt = (void *)sc->sc_cmd_resp_pkt[idx];
			sc->sc_cmd_resp_pkt[idx] = NULL;
		} else if (generation == sc->sc_generation) {
			if (sc->sc_cmd_resp_pkt[idx] != NULL) {
				kmem_intr_free(sc->sc_cmd_resp_pkt[idx],
				    sc->sc_cmd_resp_len[idx]);
				sc->sc_cmd_resp_pkt[idx] = NULL;
			}
		}
	}
 out:
	splx(s);

	return err;
}

static int
iwx_send_cmd_pdu(struct iwx_softc *sc, uint32_t id, uint32_t flags,
    uint16_t len, const void *data)
{
	struct iwx_host_cmd cmd = {
		.id = id,
		.len = { len, },
		.data = { data, },
		.flags = flags,
	};

	return iwx_send_cmd(sc, &cmd);
}

static int
iwx_send_cmd_status(struct iwx_softc *sc, struct iwx_host_cmd *cmd,
    uint32_t *status)
{
	struct iwx_rx_packet *pkt;
	struct iwx_cmd_response *resp;
	int err, resp_len;

	KASSERT((cmd->flags & IWX_CMD_WANT_RESP) == 0);
	cmd->flags |= IWX_CMD_WANT_RESP;
	cmd->resp_pkt_len = sizeof(*pkt) + sizeof(*resp);

	err = iwx_send_cmd(sc, cmd);
	if (err)
		return err;

	pkt = cmd->resp_pkt;
	if (pkt == NULL || (pkt->hdr.flags & IWX_CMD_FAILED_MSK)) {
		if (pkt != NULL)
			iwx_free_resp(sc, cmd);
		return EIO;
	}

	resp_len = iwx_rx_packet_payload_len(pkt);
	if (resp_len != sizeof(*resp)) {
		iwx_free_resp(sc, cmd);
		return EIO;
	}

	resp = (void *)pkt->data;
	*status = le32toh(resp->status);
	iwx_free_resp(sc, cmd);
	return err;
}

static int
iwx_send_cmd_pdu_status(struct iwx_softc *sc, uint32_t id, uint16_t len,
    const void *data, uint32_t *status)
{
	struct iwx_host_cmd cmd = {
		.id = id,
		.len = { len, },
		.data = { data, },
	};

	return iwx_send_cmd_status(sc, &cmd, status);
}

static void
iwx_free_resp(struct iwx_softc *sc, struct iwx_host_cmd *hcmd)
{
	KASSERT((hcmd->flags & (IWX_CMD_WANT_RESP)) == IWX_CMD_WANT_RESP);
	if (hcmd->resp_pkt != NULL)
		kmem_intr_free(hcmd->resp_pkt, hcmd->resp_pkt_len);
	hcmd->resp_pkt = NULL;
}

static void
iwx_cmd_done(struct iwx_softc *sc, int qid, int idx, int code)
{
	struct iwx_tx_ring *ring = &sc->txq[IWX_DQA_CMD_QUEUE];
	struct iwx_tx_data *data;

	if (qid != IWX_DQA_CMD_QUEUE) {
		return;	/* Not a command ack. */
	}

	data = &ring->data[idx];

	if (data->m != NULL) {
		bus_dmamap_sync(sc->sc_dmat, data->map, 0,
		    data->map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, data->map);
		m_freem(data->m);
		data->m = NULL;
	}
	wakeup(&ring->desc[idx]);

	DPRINTFN(3, ("%s: command 0x%x done\n", __func__, code));
	if (ring->queued == 0) {
		DPRINTF(("%s: unexpected firmware response to command 0x%x\n",
			DEVNAME(sc), code));
	} else if (ring->queued > 0)
		ring->queued--;
}

static uint32_t
iwx_fw_rateidx_ofdm(uint8_t rval)
{
	/* Firmware expects indices which match our 11a rate set. */
	const struct ieee80211_rateset *rs = &ieee80211_std_rateset_11a;
	int i;

	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_VAL) == rval)
			return i;
	}

	return 0;
}

static uint32_t
iwx_fw_rateidx_cck(uint8_t rval)
{
	/* Firmware expects indices which match our 11b rate set. */
	const struct ieee80211_rateset *rs = &ieee80211_std_rateset_11b;
	int i;

	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_VAL) == rval)
			return i;
	}

	return 0;
}

static int
iwx_rval2ridx(int rval)
{
	int ridx;

	for (ridx = 0; ridx < nitems(iwx_rates); ridx++) {
		if (iwx_rates[ridx].plcp == IWX_RATE_INVM_PLCP)
			continue;
		if (rval == iwx_rates[ridx].rate)
			break;
	}

	return ridx;
}

/*
 * Return the lowest basic rate of the current BSS (OpenBSD's
 * ieee80211_min_basic_rate()).
 */
static int
iwx_min_basic_rate(struct ieee80211com *ic)
{
	struct ieee80211_node *ni = ic->ic_bss;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	int i, min, rval;

	min = -1;
	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_BASIC) == 0)
			continue;
		rval = (rs->rs_rates[i] & IEEE80211_RATE_VAL);
		if (min == -1 || rval < min)
			min = rval;
	}

	if (min == -1) {
		/* No basic rates; use the lowest supported rate. */
		if (rs->rs_nrates > 0)
			min = (rs->rs_rates[0] & IEEE80211_RATE_VAL);
		else if (ni->ni_chan != IEEE80211_CHAN_ANYC &&
		    IEEE80211_IS_CHAN_5GHZ(ni->ni_chan))
			min = 12;
		else
			min = 2;
	}

	return min;
}

/*
 * Determine the Tx command flags and Tx rate+flags to use.
 * Return the selected Tx rate.
 */
static const struct iwx_rate *
iwx_tx_fill_cmd(struct iwx_softc *sc, struct iwx_node *in,
    struct ieee80211_frame *wh, uint16_t *flags, uint32_t *rate_n_flags)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = &in->in_ni;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	const struct iwx_rate *rinfo;
	int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	int min_ridx = iwx_rval2ridx(iwx_min_basic_rate(ic));
	int ridx, rate_flags;
	uint8_t rval;

	*flags = 0;

	if (min_ridx > IWX_RIDX_MAX)
		min_ridx = IWX_RIDX_CCK;

	if (IEEE80211_IS_MULTICAST(wh->i_addr1) ||
	    type != IEEE80211_FC0_TYPE_DATA) {
		/* for non-data, use the lowest supported rate */
		ridx = min_ridx;
		*flags |= IWX_TX_FLAGS_CMD_RATE;
	} else {
		if (ni->ni_txrate < rs->rs_nrates)
			rval = (rs->rs_rates[ni->ni_txrate] & IEEE80211_RATE_VAL);
		else
			rval = 2;
		ridx = iwx_rval2ridx(rval);
		if (ridx < min_ridx || ridx > IWX_RIDX_MAX)
			ridx = min_ridx;
	}

	rinfo = &iwx_rates[ridx];

	/*
	 * Do not fill rate_n_flags if firmware controls the Tx rate.
	 * For data frames we rely on Tx rate scaling in firmware by default.
	 */
	if ((*flags & IWX_TX_FLAGS_CMD_RATE) == 0) {
		*rate_n_flags = 0;
		return rinfo;
	}

	/*
	 * Forcing a CCK/OFDM legacy rate is important for management frames.
	 * Association will only succeed if we do this correctly.
	 */
	rate_flags = IWX_RATE_MCS_ANT_A_MSK;
	if (IWX_RIDX_IS_CCK(ridx)) {
		if (sc->sc_rate_n_flags_version >= 2)
			rate_flags |= IWX_RATE_MCS_CCK_MSK;
		else
			rate_flags |= IWX_RATE_MCS_CCK_MSK_V1;
	} else if (sc->sc_rate_n_flags_version >= 2)
		rate_flags |= IWX_RATE_MCS_LEGACY_OFDM_MSK;

	if (sc->sc_rate_n_flags_version >= 2) {
		if (rate_flags & IWX_RATE_MCS_LEGACY_OFDM_MSK) {
			rate_flags |= (iwx_fw_rateidx_ofdm(rinfo->rate) &
			    IWX_RATE_LEGACY_RATE_MSK);
		} else {
			rate_flags |= (iwx_fw_rateidx_cck(rinfo->rate) &
			    IWX_RATE_LEGACY_RATE_MSK);
		}
	} else
		rate_flags |= rinfo->plcp;

	*rate_n_flags = rate_flags;

	return rinfo;
}

static void
iwx_tx_update_byte_tbl(struct iwx_softc *sc, struct iwx_tx_ring *txq,
    int idx, uint16_t byte_cnt, uint16_t num_tbs)
{
	uint8_t filled_tfd_size, num_fetch_chunks;
	uint16_t len = byte_cnt;
	uint16_t bc_ent;

	filled_tfd_size = offsetof(struct iwx_tfh_tfd, tbs) +
			  num_tbs * sizeof(struct iwx_tfh_tb);
	/*
	 * filled_tfd_size contains the number of filled bytes in the TFD.
	 * Dividing it by 64 will give the number of chunks to fetch
	 * to SRAM- 0 for one chunk, 1 for 2 and so on.
	 * If, for example, TFD contains only 3 TBs then 32 bytes
	 * of the TFD are used, and only one chunk of 64 bytes should
	 * be fetched
	 */
	num_fetch_chunks = howmany(filled_tfd_size, 64) - 1;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		struct iwx_gen3_bc_tbl_entry *scd_bc_tbl = txq->bc_tbl.vaddr;
		/* Starting from AX210, the HW expects bytes */
		bc_ent = htole16(len | (num_fetch_chunks << 14));
		scd_bc_tbl[idx].tfd_offset = bc_ent;
	} else {
		struct iwx_agn_scd_bc_tbl *scd_bc_tbl = txq->bc_tbl.vaddr;
		/* Before AX210, the HW expects DW */
		len = howmany(len, 4);
		bc_ent = htole16(len | (num_fetch_chunks << 12));
		scd_bc_tbl->tfd_offset[idx] = bc_ent;
	}

	bus_dmamap_sync(sc->sc_dmat, txq->bc_tbl.map, 0,
	    txq->bc_tbl.map->dm_mapsize, BUS_DMASYNC_PREWRITE);
}

static int
iwx_tx(struct iwx_softc *sc, struct mbuf *m, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ni;
	struct iwx_tx_ring *ring;
	struct iwx_tx_data *data;
	struct iwx_tfh_tfd *desc;
	struct iwx_device_cmd *cmd;
	struct ieee80211_frame *wh;
	struct ieee80211_key *k = NULL;
	struct mbuf *m1;
	const struct iwx_rate *rinfo;
	uint64_t paddr;
	u_int hdrlen;
	bus_dma_segment_t *seg;
	uint32_t rate_n_flags;
	uint16_t num_tbs, flags, offload_assist = 0;
	uint8_t type;
	int i, totlen, err, pad, qid;
	size_t txcmd_size;

	wh = mtod(m, struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	if (type == IEEE80211_FC0_TYPE_CTL)
		hdrlen = sizeof(struct ieee80211_frame_min);
	else
		hdrlen = ieee80211_anyhdrsize(wh);

	/* All frames go through the management/data queue (no Tx agg). */
	qid = sc->first_data_qid;

	/*
	 * net80211 may still hand us management frames (e.g. a DISASSOC on
	 * the way to INIT state) after the station and its queue have been
	 * removed from the firmware. Transmitting on a disabled queue
	 * triggers a firmware SYSASSERT, so drop such frames.
	 */
	if ((sc->qenablemsk & (1 << qid)) == 0) {
		m_freem(m);
		return ENETDOWN;
	}

	ring = &sc->txq[qid];
	desc = &ring->desc[ring->cur];
	memset(desc, 0, sizeof(*desc));
	data = &ring->data[ring->cur];

	cmd = &ring->cmd[ring->cur];
	cmd->hdr.code = IWX_TX_CMD;
	cmd->hdr.flags = 0;
	cmd->hdr.qid = ring->qid;
	cmd->hdr.idx = ring->cur;

	rinfo = iwx_tx_fill_cmd(sc, in, wh, &flags, &rate_n_flags);

	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct iwx_tx_radiotap_header *tap = &sc->sc_txtap;
		uint16_t chan_flags;

		tap->wt_flags = 0;
		tap->wt_chan_freq = htole16(ni->ni_chan->ic_freq);
		chan_flags = ni->ni_chan->ic_flags;
		tap->wt_chan_flags = htole16(chan_flags);
		tap->wt_rate = rinfo->rate;
		if (wh->i_fc[1] & IEEE80211_FC1_WEP)
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;

		bpf_mtap2(sc->sc_drvbpf, tap, sc->sc_txtap_len, m, BPF_D_OUT);
	}

	/*
	 * Encrypt the frame in software if needed. We never install keys
	 * into the firmware, so tell it not to touch the frame.
	 */
	if (wh->i_fc[1] & IEEE80211_FC1_WEP) {
		k = ieee80211_crypto_encap(ic, ni, m);
		if (k == NULL) {
			m_freem(m);
			return ENOBUFS;
		}
		/* 802.11 header may have moved. */
		wh = mtod(m, struct ieee80211_frame *);
	}
	flags |= IWX_TX_FLAGS_ENCRYPT_DIS;

	totlen = m->m_pkthdr.len;

	offload_assist |= IWX_TX_CMD_OFFLD_MH_SIZE((hdrlen / 2) &
	    IWX_TX_CMD_OFFLD_MH_MASK);
	if (hdrlen & 3) {
		/* First segment length must be a multiple of 4. */
		pad = 4 - (hdrlen & 3);
		offload_assist |= IWX_TX_CMD_OFFLD_PAD;
	} else
		pad = 0;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		struct iwx_tx_cmd_gen3 *tx = (void *)cmd->data;
		memset(tx, 0, sizeof(*tx));
		tx->len = htole16(totlen);
		tx->offload_assist = htole32(offload_assist);
		tx->flags = htole16(flags);
		tx->rate_n_flags = htole32(rate_n_flags);
		memcpy(tx->hdr, wh, hdrlen);
		txcmd_size = sizeof(*tx);
	} else {
		struct iwx_tx_cmd_gen2 *tx = (void *)cmd->data;
		memset(tx, 0, sizeof(*tx));
		tx->len = htole16(totlen);
		tx->offload_assist = htole16(offload_assist);
		tx->flags = htole32(flags);
		tx->rate_n_flags = htole32(rate_n_flags);
		memcpy(tx->hdr, wh, hdrlen);
		txcmd_size = sizeof(*tx);
	}

	/* Trim 802.11 header. */
	m_adj(m, hdrlen);

	err = bus_dmamap_load_mbuf(sc->sc_dmat, data->map, m,
	    BUS_DMA_NOWAIT | BUS_DMA_WRITE);
	if (err && err != EFBIG) {
		printf("%s: can't map mbuf (error %d)\n", DEVNAME(sc), err);
		m_freem(m);
		return err;
	}
	if (err) {
		/* Too many DMA segments, linearize mbuf. */
		MGETHDR(m1, M_DONTWAIT, MT_DATA);
		if (m1 == NULL) {
			m_freem(m);
			return ENOBUFS;
		}
		if (m->m_pkthdr.len > MHLEN) {
			MCLGET(m1, M_DONTWAIT);
			if (!(m1->m_flags & M_EXT)) {
				m_freem(m);
				m_freem(m1);
				return ENOBUFS;
			}
		}
		m_copydata(m, 0, m->m_pkthdr.len, mtod(m1, void *));
		m1->m_pkthdr.len = m1->m_len = m->m_pkthdr.len;
		m_freem(m);
		m = m1;

		err = bus_dmamap_load_mbuf(sc->sc_dmat, data->map, m,
		    BUS_DMA_NOWAIT | BUS_DMA_WRITE);
		if (err) {
			printf("%s: can't map mbuf (error %d)\n", DEVNAME(sc),
			    err);
			m_freem(m);
			return err;
		}
	}
	data->m = m;
	data->in = in;

	/* Fill TX descriptor. */
	num_tbs = 2 + data->map->dm_nsegs;
	desc->num_tbs = htole16(num_tbs);

	desc->tbs[0].tb_len = htole16(IWX_FIRST_TB_SIZE);
	paddr = htole64(data->cmd_paddr);
	memcpy(&desc->tbs[0].addr, &paddr, sizeof(paddr));
	if ((uint64_t)data->cmd_paddr >> 32 != ((uint64_t)data->cmd_paddr +
	    le32toh(desc->tbs[0].tb_len)) >> 32)
		DPRINTF(("%s: TB0 crosses 32bit boundary\n", __func__));
	desc->tbs[1].tb_len = htole16(sizeof(struct iwx_cmd_header) +
	    txcmd_size + hdrlen + pad - IWX_FIRST_TB_SIZE);
	paddr = htole64(data->cmd_paddr + IWX_FIRST_TB_SIZE);
	memcpy(&desc->tbs[1].addr, &paddr, sizeof(paddr));

	if ((uint64_t)data->cmd_paddr >> 32 != ((uint64_t)data->cmd_paddr +
	    le32toh(desc->tbs[1].tb_len)) >> 32)
		DPRINTF(("%s: TB1 crosses 32bit boundary\n", __func__));

	/* Other DMA segments are for data payload. */
	seg = data->map->dm_segs;
	for (i = 0; i < data->map->dm_nsegs; i++, seg++) {
		desc->tbs[i + 2].tb_len = htole16(seg->ds_len);
		paddr = htole64(seg->ds_addr);
		memcpy(&desc->tbs[i + 2].addr, &paddr, sizeof(paddr));
		if ((uint64_t)data->cmd_paddr >> 32 !=
		    ((uint64_t)data->cmd_paddr +
		    le32toh(desc->tbs[i + 2].tb_len)) >> 32)
			DPRINTF(("%s: TB%d crosses 32bit boundary\n", __func__, i + 2));
	}

	bus_dmamap_sync(sc->sc_dmat, data->map, 0, data->map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(sc->sc_dmat, ring->cmd_dma.map,
	    (char *)(void *)cmd - (char *)(void *)ring->cmd_dma.vaddr,
	    sizeof (*cmd), BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(sc->sc_dmat, ring->desc_dma.map,
	    (char *)(void *)desc - (char *)(void *)ring->desc_dma.vaddr,
	    sizeof (*desc), BUS_DMASYNC_PREWRITE);

	iwx_tx_update_byte_tbl(sc, ring, ring->cur, totlen, num_tbs);

	/* Kick TX ring. */
	ring->cur = (ring->cur + 1) % IWX_TX_RING_COUNT;
	ring->cur_hw = (ring->cur_hw + 1) % sc->max_tfd_queue_size;
	IWX_WRITE(sc, IWX_HBUS_TARG_WRPTR, ring->qid << 16 | ring->cur_hw);

	/* Mark TX ring as full if we reach a certain threshold. */
	if (++ring->queued > IWX_TX_RING_HIMARK) {
		sc->qfullmsk |= 1 << ring->qid;
	}

	if (IC2IFP(ic)->if_flags & IFF_UP)
		sc->sc_tx_timer[ring->qid] = 15;

	return 0;
}

static int
iwx_flush_sta_tids(struct iwx_softc *sc, int sta_id, uint16_t tids)
{
	struct iwx_rx_packet *pkt;
	struct iwx_tx_path_flush_cmd_rsp *resp;
	struct iwx_tx_path_flush_cmd flush_cmd = {
		.sta_id = htole32(sta_id),
		.tid_mask = htole16(tids),
	};
	struct iwx_host_cmd hcmd = {
		.id = IWX_TXPATH_FLUSH,
		.len = { sizeof(flush_cmd), },
		.data = { &flush_cmd, },
		.flags = IWX_CMD_WANT_RESP,
		.resp_pkt_len = sizeof(*pkt) + sizeof(*resp),
	};
	int err, resp_len, i, num_flushed_queues;

	err = iwx_send_cmd(sc, &hcmd);
	if (err)
		return err;

	pkt = hcmd.resp_pkt;
	if (!pkt || (pkt->hdr.flags & IWX_CMD_FAILED_MSK)) {
		err = EIO;
		goto out;
	}

	resp_len = iwx_rx_packet_payload_len(pkt);
	if (resp_len != sizeof(*resp)) {
		err = EIO;
		goto out;
	}

	resp = (void *)pkt->data;

	if (le16toh(resp->sta_id) != sta_id) {
		err = EIO;
		goto out;
	}

	num_flushed_queues = le16toh(resp->num_flushed_queues);
	if (num_flushed_queues > IWX_TX_FLUSH_QUEUE_RSP) {
		err = EIO;
		goto out;
	}

	for (i = 0; i < num_flushed_queues; i++) {
		struct iwx_flush_queue_info *queue_info = &resp->queues[i];
		uint16_t tid = le16toh(queue_info->tid);
		uint16_t read_after = le16toh(queue_info->read_after_flush);
		uint16_t qid = le16toh(queue_info->queue_num);
		struct iwx_tx_ring *txq;

		if (qid >= nitems(sc->txq))
			continue;

		txq = &sc->txq[qid];
		if (tid != txq->tid)
			continue;

		iwx_txq_advance(sc, txq, read_after);
	}
out:
	iwx_free_resp(sc, &hcmd);
	return err;
}

#define IWX_FLUSH_WAIT_MS	2000

static int
iwx_drain_sta(struct iwx_softc *sc, struct iwx_node* in, int drain)
{
	struct iwx_add_sta_cmd cmd;
	int err;
	uint32_t status;

	/* No need to drain with MLD firmware. */
	if (sc->sc_use_mld_api)
		return 0;
	
	memset(&cmd, 0, sizeof(cmd));
	cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id,
	    in->in_color));
	cmd.sta_id = IWX_STATION_ID;
	cmd.add_modify = IWX_STA_MODE_MODIFY;
	cmd.station_flags = drain ? htole32(IWX_STA_FLG_DRAIN_FLOW) : 0;
	cmd.station_flags_msk = htole32(IWX_STA_FLG_DRAIN_FLOW);

	status = IWX_ADD_STA_SUCCESS;
	err = iwx_send_cmd_pdu_status(sc, IWX_ADD_STA,
	    sizeof(cmd), &cmd, &status);
	if (err) {
		printf("%s: could not update sta (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	switch (status & IWX_ADD_STA_STATUS_MASK) {
	case IWX_ADD_STA_SUCCESS:
		break;
	default:
		err = EIO;
		printf("%s: Couldn't %s draining for station\n",
		    DEVNAME(sc), drain ? "enable" : "disable");
		break;
	}

	return err;
}

static int
iwx_flush_sta(struct iwx_softc *sc, struct iwx_node *in)
{
	int err;

	sc->sc_flags |= IWX_FLAG_TXFLUSH;

	err = iwx_drain_sta(sc, in, 1);
	if (err)
		goto done;

	err = iwx_flush_sta_tids(sc, IWX_STATION_ID, 0xffff);
	if (err) {
		printf("%s: could not flush Tx path (error %d)\n",
		    DEVNAME(sc), err);
		goto done;
	}

	err = iwx_drain_sta(sc, in, 0);
done:
	sc->sc_flags &= ~IWX_FLAG_TXFLUSH;
	return err;
}

#define IWX_POWER_KEEP_ALIVE_PERIOD_SEC    25

static int
iwx_beacon_filter_send_cmd(struct iwx_softc *sc,
    struct iwx_beacon_filter_cmd *cmd)
{
	return iwx_send_cmd_pdu(sc, IWX_REPLY_BEACON_FILTERING_CMD,
	    0, sizeof(struct iwx_beacon_filter_cmd), cmd);
}

static int
iwx_update_beacon_abort(struct iwx_softc *sc, struct iwx_node *in, int enable)
{
	struct iwx_beacon_filter_cmd cmd = {
		IWX_BF_CMD_CONFIG_DEFAULTS,
		.bf_enable_beacon_filter = htole32(1),
		.ba_enable_beacon_abort = htole32(enable),
	};

	if (!sc->sc_bf.bf_enabled)
		return 0;

	sc->sc_bf.ba_enabled = enable;
	return iwx_beacon_filter_send_cmd(sc, &cmd);
}

static int
iwx_set_pslevel(struct iwx_softc *sc, int dtim, int level, int async)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_device_power_cmd dcmd = { };
	struct iwx_mac_power_cmd mcmd;
	struct iwx_node *in;
	struct ieee80211_node *ni;
	const struct iwx_pmgt *pmgt;
	int range, skip_dtim, cmd_flags, err;
	int dtim_period, dtim_msec, keep_alive;

	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		return 0;

	if (dtim == 0) {
		dtim = 1;
		skip_dtim = 0;
	} else
		skip_dtim = -1;

	if (dtim <= 2)
		range = 0;
	else if (dtim <= 10)
		range = 1;
	else
		range = 2;

	pmgt = &iwx_pmgt[range][level];
	if (skip_dtim == -1)
		skip_dtim = pmgt->skip_dtim;

	if (level != 0)
		dcmd.flags = htole16(IWX_DEVICE_POWER_FLAGS_POWER_SAVE_ENA_MSK);

	cmd_flags = async ? IWX_CMD_ASYNC : 0;
	err = iwx_send_cmd_pdu(sc, IWX_POWER_TABLE_CMD, cmd_flags,
	    sizeof(dcmd), &dcmd);
	if (err)
		return err;

	if ((sc->sc_flags & IWX_FLAG_MAC_ACTIVE) == 0)
		return 0;

	in = (void *)ic->ic_bss;
	ni = &in->in_ni;

	memset(&mcmd, 0, sizeof(mcmd));
	mcmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id,
	    in->in_color));
	dtim_period = ni->ni_dtim_period ? ni->ni_dtim_period : 1;
	dtim_msec = dtim_period * ni->ni_intval;
	keep_alive = MAX(3 * dtim_msec, 1000 * IWX_POWER_KEEP_ALIVE_PERIOD_SEC);
	keep_alive = roundup(keep_alive, 1000) / 1000;
	mcmd.keep_alive_seconds = htole16(keep_alive);

	if (level != 0) {
		mcmd.flags = htole16(IWX_POWER_FLAGS_POWER_SAVE_ENA_MSK |
		    IWX_POWER_FLAGS_POWER_MANAGEMENT_ENA_MSK);
		mcmd.rx_data_timeout = htole32(pmgt->rxtimeout * 1024);
		mcmd.tx_data_timeout = htole32(pmgt->txtimeout * 1024);
		if (skip_dtim != 0) {
			mcmd.flags |= htole16(IWX_POWER_FLAGS_SKIP_OVER_DTIM_MSK);
			mcmd.skip_dtim_periods = skip_dtim + 1;
		}
	}

	err = iwx_send_cmd_pdu(sc, IWX_MAC_PM_POWER_TABLE, cmd_flags,
	    sizeof(mcmd), &mcmd);
	if (err != 0)
		return err;

	return iwx_update_beacon_abort(sc, in, !!(mcmd.flags &
	    htole16(IWX_POWER_FLAGS_POWER_MANAGEMENT_ENA_MSK)));
}

static int
iwx_disable_beacon_filter(struct iwx_softc *sc)
{
	struct iwx_beacon_filter_cmd cmd;
	int err;

	memset(&cmd, 0, sizeof(cmd));

	err = iwx_beacon_filter_send_cmd(sc, &cmd);
	if (err == 0)
		sc->sc_bf.bf_enabled = 0;

	return err;
}

static int
iwx_add_sta_cmd(struct iwx_softc *sc, struct iwx_node *in, int update)
{
	struct iwx_add_sta_cmd add_sta_cmd;
	int err;
	uint32_t status;
	struct ieee80211com *ic = &sc->sc_ic;

	if (!update && (sc->sc_flags & IWX_FLAG_STA_ACTIVE))
		panic("STA already added");

	if (sc->sc_use_mld_api)
		return iwx_mld_add_sta_cmd(sc, in, update);

	memset(&add_sta_cmd, 0, sizeof(add_sta_cmd));

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		add_sta_cmd.sta_id = IWX_MONITOR_STA_ID;
		add_sta_cmd.station_type = IWX_STA_GENERAL_PURPOSE;
	} else {
		add_sta_cmd.sta_id = IWX_STATION_ID;
		add_sta_cmd.station_type = IWX_STA_LINK;
	}
	add_sta_cmd.mac_id_n_color
	    = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id, in->in_color));
	if (!update) {
		if (ic->ic_opmode == IEEE80211_M_MONITOR)
			IEEE80211_ADDR_COPY(&add_sta_cmd.addr,
			    iwx_etheranyaddr);
		else
			IEEE80211_ADDR_COPY(&add_sta_cmd.addr,
			    in->in_macaddr);
	}
	add_sta_cmd.add_modify = update ? 1 : 0;
	add_sta_cmd.station_flags_msk
	    |= htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);

	status = IWX_ADD_STA_SUCCESS;
	err = iwx_send_cmd_pdu_status(sc, IWX_ADD_STA, sizeof(add_sta_cmd),
	    &add_sta_cmd, &status);
	if (!err && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS)
		err = EIO;

	return err;
}

/*
 * Fill in EDCA parameters for the given access category. net80211 only
 * negotiates WME parameters when IEEE80211_C_WME is set, which this
 * driver does not advertise, so fall back to the 802.11 defaults.
 */
static void
iwx_get_edca_params(struct iwx_softc *sc, int ac, uint16_t *cw_min,
    uint16_t *cw_max, uint8_t *aifsn, uint16_t *txop)
{
#define IWX_EXP2(x)	((1 << (x)) - 1)	/* CWmin = 2^ECWmin - 1 */
	struct ieee80211com *ic = &sc->sc_ic;
	const struct wmeParams *wmep =
	    &ic->ic_wme.wme_chanParams.cap_wmeParams[ac];

	if ((ic->ic_caps & IEEE80211_C_WME) && wmep->wmep_logcwmax != 0) {
		*cw_min = IWX_EXP2(wmep->wmep_logcwmin);
		*cw_max = IWX_EXP2(wmep->wmep_logcwmax);
		*aifsn = wmep->wmep_aifsn;
		*txop = wmep->wmep_txopLimit;
	} else {
		*cw_min = IWX_EXP2(iwx_edca_defaults[ac].ecwmin);
		*cw_max = IWX_EXP2(iwx_edca_defaults[ac].ecwmax);
		*aifsn = iwx_edca_defaults[ac].aifsn;
		*txop = iwx_edca_defaults[ac].txop;
	}
#undef IWX_EXP2
}

static void
iwx_mld_modify_link_fill(struct iwx_softc *sc, struct iwx_node *in,
    struct iwx_link_config_cmd *cmd, int changes, int active)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = &in->in_ni;
	int cck_ack_rates, ofdm_ack_rates;
	int i;

	cmd->link_id = htole32(0);
	cmd->mac_id = htole32(in->in_id);
	cmd->phy_id = htole32(in->in_phyctxt != NULL ?
	    in->in_phyctxt->id : sc->sc_phyctxt[0].id);
	IEEE80211_ADDR_COPY(cmd->local_link_addr, ic->ic_myaddr);
	cmd->active = htole32(active);

	iwx_ack_rates(sc, in, &cck_ack_rates, &ofdm_ack_rates);
	cmd->cck_rates = htole32(cck_ack_rates);
	cmd->ofdm_rates = htole32(ofdm_ack_rates);
	cmd->cck_short_preamble
	    = htole32((ic->ic_flags & IEEE80211_F_SHPREAMBLE) ? 1 : 0);
	cmd->short_slot
	    = htole32((ic->ic_flags & IEEE80211_F_SHSLOT) ? 1 : 0);

	for (i = 0; i < WME_NUM_AC; i++) {
		uint16_t cw_min, cw_max, txop;
		uint8_t aifsn;
		int txf;

		if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
			txf = iwx_ac_to_bz_tx_fifo[i];
		else
			txf = iwx_ac_to_tx_fifo[i];

		iwx_get_edca_params(sc, i, &cw_min, &cw_max, &aifsn, &txop);
		cmd->ac[txf].cw_min = htole16(cw_min);
		cmd->ac[txf].cw_max = htole16(cw_max);
		cmd->ac[txf].aifsn = aifsn;
		cmd->ac[txf].fifos_mask = (1 << txf);
		cmd->ac[txf].edca_txop = htole16(txop * 32);
	}
	if (ni->ni_flags & IEEE80211_NODE_QOS)
		cmd->qos_flags |= htole32(IWX_MAC_QOS_FLG_UPDATE_EDCA);

	if (ic->ic_flags & IEEE80211_F_USEPROT)
		cmd->protection_flags |= htole32(IWX_LINK_PROT_FLG_TGG_PROTECT);

	cmd->bi = htole32(ni->ni_intval);
	cmd->dtim_interval = htole32(ni->ni_intval * ni->ni_dtim_period);

	cmd->modify_mask = htole32(changes);
	cmd->flags = 0;
	cmd->flags_mask = 0;
	cmd->spec_link_id = 0;
	cmd->listen_lmac = 0;
	cmd->action = IWX_FW_CTXT_ACTION_MODIFY;
}

static int
iwx_mld_add_sta_cmd(struct iwx_softc *sc, struct iwx_node *in, int update)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_link_config_cmd link_cmd;
	struct iwx_sta_cfg_cmd_v2 sta_cmd;
	int cmd_ver;
	size_t cmd_size;
	int err, changes;

	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_MAC_CONF_GROUP,
	    IWX_STA_CONFIG_CMD);
	switch (cmd_ver) {
	case 2:
		cmd_size = sizeof(sta_cmd);
		break;
	case 1:
	case IWX_FW_CMD_VER_UNKNOWN:
		/* v1 is a shorter variant of v2 */
		cmd_size = sizeof(struct iwx_mvm_sta_cfg_cmd);
		break;
	default:
		printf("%s: unsupported STA_CONFIG_CMD version %d\n",
		    DEVNAME(sc), cmd_ver);
		return ENOTSUP;
	}
		
	if (!update) {
		memset(&link_cmd, 0, sizeof(link_cmd));
		link_cmd.link_id = htole32(0);
		link_cmd.mac_id = htole32(in->in_id);
		link_cmd.spec_link_id = 0;
		if (in->in_phyctxt)
			link_cmd.phy_id = htole32(in->in_phyctxt->id);
		else
			link_cmd.phy_id = htole32(IWX_FW_CTXT_INVALID);
		IEEE80211_ADDR_COPY(link_cmd.local_link_addr, ic->ic_myaddr);
		link_cmd.listen_lmac = 0;
		link_cmd.action = IWX_FW_CTXT_ACTION_ADD;

		err = iwx_send_cmd_pdu(sc,
		    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD),
		    0, sizeof(link_cmd), &link_cmd);
		if (err)
			return err;
	}

	changes = IWX_LINK_CONTEXT_MODIFY_ACTIVE;
	changes |= IWX_LINK_CONTEXT_MODIFY_RATES_INFO;
	if (update) {
		changes |= IWX_LINK_CONTEXT_MODIFY_PROTECT_FLAGS;
		changes |= IWX_LINK_CONTEXT_MODIFY_QOS_PARAMS;
		changes |= IWX_LINK_CONTEXT_MODIFY_BEACON_TIMING;
	}

	memset(&link_cmd, 0, sizeof(link_cmd));
	iwx_mld_modify_link_fill(sc, in, &link_cmd, changes, 1);
	err = iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD),
	    0, sizeof(link_cmd), &link_cmd);
	if (err)
		return err;

	memset(&sta_cmd, 0, sizeof(sta_cmd));
	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		sta_cmd.sta_id = htole32(IWX_MONITOR_STA_ID);
		sta_cmd.station_type = htole32(IWX_STA_GENERAL_PURPOSE);
	} else {
		sta_cmd.sta_id = htole32(IWX_STATION_ID);
		sta_cmd.station_type = htole32(IWX_STA_LINK);
	}
	sta_cmd.link_id = htole32(0);
	IEEE80211_ADDR_COPY(sta_cmd.peer_mld_address, in->in_macaddr);
	IEEE80211_ADDR_COPY(sta_cmd.peer_link_address, in->in_macaddr);
	sta_cmd.assoc_id = htole32(IEEE80211_AID(in->in_ni.ni_associd));

	return iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD),
	    0, cmd_size, &sta_cmd);
}

static int
iwx_rm_sta_cmd(struct iwx_softc *sc, struct iwx_node *in)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_rm_sta_cmd rm_sta_cmd;
	int err;

	if ((sc->sc_flags & IWX_FLAG_STA_ACTIVE) == 0)
		panic("sta already removed");

	if (sc->sc_use_mld_api)
		return iwx_mld_rm_sta_cmd(sc, in);

	memset(&rm_sta_cmd, 0, sizeof(rm_sta_cmd));
	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		rm_sta_cmd.sta_id = IWX_MONITOR_STA_ID;
	else
		rm_sta_cmd.sta_id = IWX_STATION_ID;

	err = iwx_send_cmd_pdu(sc, IWX_REMOVE_STA, 0, sizeof(rm_sta_cmd),
	    &rm_sta_cmd);

	return err;
}

static int
iwx_rm_sta(struct iwx_softc *sc, struct iwx_node *in)
{
	int err, i, cmd_ver;

	err = iwx_flush_sta(sc, in);
	if (err) {
		printf("%s: could not flush Tx path (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	/*
	 * New SCD_QUEUE_CONFIG API requires explicit queue removal
	 * before a station gets removed.
	 */
	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_SCD_QUEUE_CONFIG_CMD);
	if (cmd_ver != 0 && cmd_ver != IWX_FW_CMD_VER_UNKNOWN) {
		err = iwx_disable_mgmt_queue(sc);
		if (err)
			return err;
		for (i = IWX_FIRST_AGG_TX_QUEUE;
		    i < IWX_LAST_AGG_TX_QUEUE; i++) {
			struct iwx_tx_ring *ring = &sc->txq[i];
			if ((sc->qenablemsk & (1 << i)) == 0)
				continue;
			err = iwx_disable_txq(sc, IWX_STATION_ID,
			    ring->qid, ring->tid);
			if (err) {
				printf("%s: could not disable Tx queue %d "
				    "(error %d)\n", DEVNAME(sc), ring->qid,
				    err);
				return err;
			}
		}
	}

	err = iwx_rm_sta_cmd(sc, in);
	if (err) {
		printf("%s: could not remove STA (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	in->in_flags = 0;

	sc->sc_rx_ba_sessions = 0;
	memset(sc->aggqid, 0, sizeof(sc->aggqid));
	for (i = IWX_FIRST_AGG_TX_QUEUE; i < IWX_LAST_AGG_TX_QUEUE; i++)
		sc->qenablemsk &= ~(1 << i);

	return 0;
}

static int
iwx_mld_rm_sta_cmd(struct iwx_softc *sc, struct iwx_node *in)
{
	struct iwx_mvm_remove_sta_cmd sta_cmd;
	struct iwx_link_config_cmd link_cmd;
	int err;

	memset(&sta_cmd, 0, sizeof(sta_cmd));
	sta_cmd.sta_id = htole32(IWX_STATION_ID);

	err = iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_REMOVE_CMD),
	    0, sizeof(sta_cmd), &sta_cmd);
	if (err)
		return err;

	memset(&link_cmd, 0, sizeof(link_cmd));
	iwx_mld_modify_link_fill(sc, in, &link_cmd,
	    IWX_LINK_CONTEXT_MODIFY_ACTIVE, 0);
	err = iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD),
	    0, sizeof(link_cmd), &link_cmd);
	if (err)
		return err;

	memset(&link_cmd, 0, sizeof(link_cmd));
	link_cmd.link_id = htole32(0);
	link_cmd.spec_link_id = 0;
	link_cmd.action = IWX_FW_CTXT_ACTION_REMOVE;

	return iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD),
	    0, sizeof(link_cmd), &link_cmd);
}

static uint8_t
iwx_umac_scan_fill_channels(struct iwx_softc *sc,
    struct iwx_scan_channel_cfg_umac *chan, size_t chan_nitems,
    int n_ssids, uint32_t channel_cfg_flags)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c;
	uint8_t nchan;

	for (nchan = 0, c = &ic->ic_channels[1];
	    c <= &ic->ic_channels[IEEE80211_CHAN_MAX] &&
	    nchan < chan_nitems &&
	    nchan < sc->sc_capa_n_scan_channels;
	    c++) {
		uint8_t channel_num;

		if (c->ic_flags == 0)
			continue;

		channel_num = ieee80211_mhz2ieee(c->ic_freq, 0);
		if (isset(sc->sc_ucode_api,
		    IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER)) {
			chan->v2.channel_num = channel_num;
			if (IEEE80211_IS_CHAN_2GHZ(c))
				chan->v2.band = IWX_PHY_BAND_24;
			else
				chan->v2.band = IWX_PHY_BAND_5;
			chan->v2.iter_count = 1;
			chan->v2.iter_interval = 0;
		} else {
			chan->v1.channel_num = channel_num;
			chan->v1.iter_count = 1;
			chan->v1.iter_interval = htole16(0);
		}

		chan->flags = htole32(channel_cfg_flags);
		chan++;
		nchan++;
	}

	return nchan;
}

static uint8_t
iwx_umac_scan_fill_channels_v5(struct iwx_softc *sc,
    struct iwx_scan_channel_cfg_umac_v5 *chan, size_t chan_nitems,
    int n_ssids, uint32_t channel_cfg_flags)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c;
	uint8_t nchan;

	for (nchan = 0, c = &ic->ic_channels[1];
	    c <= &ic->ic_channels[IEEE80211_CHAN_MAX] &&
	    nchan < chan_nitems &&
	    nchan < sc->sc_capa_n_scan_channels;
	    c++) {
		uint8_t channel_num, band;

		if (c->ic_flags == 0)
			continue;

		channel_num = ieee80211_mhz2ieee(c->ic_freq, 0);
		if (IEEE80211_IS_CHAN_2GHZ(c))
			band = IWX_PHY_BAND_24;
		else
			band = IWX_PHY_BAND_5;

		chan->channel_num = channel_num;
		chan->psd_20 = -128;
		chan->iter_count = 1;
		chan->iter_interval = 0;

		chan->flags = htole32(channel_cfg_flags |
		    (band << IWX_CHAN_CFG_FLAGS_BAND_POS));
		chan++;
		nchan++;
	}

	return nchan;
}

static int
iwx_fill_probe_req(struct iwx_softc *sc, struct iwx_scan_probe_req *preq)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh = (struct ieee80211_frame *)preq->buf;
	struct ieee80211_rateset *rs;
	size_t remain = sizeof(preq->buf);
	uint8_t *frm, *pos;

	memset(preq, 0, sizeof(*preq));

	if (remain < sizeof(*wh) + 2)
		return ENOBUFS;

	/*
	 * Build a probe request frame.  Most of the following code is a
	 * copy & paste of what is done in net80211.
	 */
	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_MGT |
	    IEEE80211_FC0_SUBTYPE_PROBE_REQ;
	wh->i_fc[1] = IEEE80211_FC1_DIR_NODS;
	IEEE80211_ADDR_COPY(wh->i_addr1, etherbroadcastaddr);
	IEEE80211_ADDR_COPY(wh->i_addr2, ic->ic_myaddr);
	IEEE80211_ADDR_COPY(wh->i_addr3, etherbroadcastaddr);
	*(uint16_t *)&wh->i_dur[0] = 0;	/* filled by HW */
	*(uint16_t *)&wh->i_seq[0] = 0;	/* filled by HW */

	frm = (uint8_t *)(wh + 1);
	*frm++ = IEEE80211_ELEMID_SSID;
	*frm++ = 0;
	/* hardware inserts SSID */

	/* Tell the firmware where the MAC header is. */
	preq->mac_header.offset = 0;
	preq->mac_header.len = htole16(frm - (uint8_t *)wh);
	remain -= frm - (uint8_t *)wh;

	/* Fill in 2GHz IEs and tell firmware where they are. */
	rs = &ic->ic_sup_rates[IEEE80211_MODE_11G];
	if (rs->rs_nrates > IEEE80211_RATE_SIZE) {
		if (remain < (size_t)(4 + rs->rs_nrates))
			return ENOBUFS;
	} else if (remain < (size_t)(2 + rs->rs_nrates))
		return ENOBUFS;
	preq->band_data[0].offset = htole16(frm - (uint8_t *)wh);
	pos = frm;
	frm = ieee80211_add_rates(frm, rs);
	if (rs->rs_nrates > IEEE80211_RATE_SIZE)
		frm = ieee80211_add_xrates(frm, rs);
	remain -= frm - pos;

	if (isset(sc->sc_enabled_capa,
	    IWX_UCODE_TLV_CAPA_DS_PARAM_SET_IE_SUPPORT)) {
		if (remain < 3)
			return ENOBUFS;
		*frm++ = IEEE80211_ELEMID_DSPARMS;
		*frm++ = 1;
		*frm++ = 0;
		remain -= 3;
	}
	preq->band_data[0].len = htole16(frm - pos);

	if (sc->sc_nvm.sku_cap_band_52GHz_enable) {
		/* Fill in 5GHz IEs. */
		rs = &ic->ic_sup_rates[IEEE80211_MODE_11A];
		if (rs->rs_nrates > IEEE80211_RATE_SIZE) {
			if (remain < (size_t)(4 + rs->rs_nrates))
				return ENOBUFS;
		} else if (remain < (size_t)(2 + rs->rs_nrates))
			return ENOBUFS;
		preq->band_data[1].offset = htole16(frm - (uint8_t *)wh);
		pos = frm;
		frm = ieee80211_add_rates(frm, rs);
		if (rs->rs_nrates > IEEE80211_RATE_SIZE)
			frm = ieee80211_add_xrates(frm, rs);
		preq->band_data[1].len = htole16(frm - pos);
		remain -= frm - pos;
	}

	/* No 11n IEs: HT is not supported by this port. */
	preq->common_data.offset = htole16(frm - (uint8_t *)wh);
	preq->common_data.len = htole16(0);

	return 0;
}

static int
iwx_config_umac_scan_reduced(struct iwx_softc *sc)
{
	struct iwx_scan_config scan_cfg;
	struct iwx_host_cmd hcmd = {
		.id = iwx_cmd_id(IWX_SCAN_CFG_CMD, IWX_LONG_GROUP, 0),
		.len[0] = sizeof(scan_cfg),
		.data[0] = &scan_cfg,
		.flags = 0,
	};
	int cmdver;

	if (!isset(sc->sc_ucode_api, IWX_UCODE_TLV_API_REDUCED_SCAN_CONFIG)) {
		printf("%s: firmware does not support reduced scan config\n",
		    DEVNAME(sc));
		return ENOTSUP;
	}

	memset(&scan_cfg, 0, sizeof(scan_cfg));

	/*
	 * SCAN_CFG version >= 5 implies that the broadcast
	 * STA ID field is deprecated.
	 */
	cmdver = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_SCAN_CFG_CMD);
	if (cmdver == IWX_FW_CMD_VER_UNKNOWN || cmdver < 5)
		scan_cfg.bcast_sta_id = 0xff;

	scan_cfg.tx_chains = htole32(iwx_fw_valid_tx_ant(sc));
	scan_cfg.rx_chains = htole32(iwx_fw_valid_rx_ant(sc));

	return iwx_send_cmd(sc, &hcmd);
}

static uint16_t
iwx_scan_umac_flags_v2(struct iwx_softc *sc, int bgscan)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint16_t flags = 0;

	if (ic->ic_des_esslen == 0)
		flags |= IWX_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE;

	flags |= IWX_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL;
	flags |= IWX_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE;
	flags |= IWX_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL;

	return flags;
}

#define IWX_SCAN_DWELL_ACTIVE		10
#define IWX_SCAN_DWELL_PASSIVE		110

/* adaptive dwell max budget time [TU] for full scan */
#define IWX_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN 300
/* adaptive dwell max budget time [TU] for directed scan */
#define IWX_SCAN_ADWELL_MAX_BUDGET_DIRECTED_SCAN 100
/* adaptive dwell default high band APs number */
#define IWX_SCAN_ADWELL_DEFAULT_HB_N_APS 8
/* adaptive dwell default low band APs number */
#define IWX_SCAN_ADWELL_DEFAULT_LB_N_APS 2
/* adaptive dwell default APs number in social channels (1, 6, 11) */
#define IWX_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL 10
/* adaptive dwell number of APs override for p2p friendly GO channels */
#define IWX_SCAN_ADWELL_N_APS_GO_FRIENDLY 10
/* adaptive dwell number of APs override for social channels */
#define IWX_SCAN_ADWELL_N_APS_SOCIAL_CHS 2

static void
iwx_scan_umac_dwell_v10(struct iwx_softc *sc,
    struct iwx_scan_general_params_v10 *general_params, int bgscan)
{
	uint32_t suspend_time, max_out_time;
	uint8_t active_dwell, passive_dwell;

	active_dwell = IWX_SCAN_DWELL_ACTIVE;
	passive_dwell = IWX_SCAN_DWELL_PASSIVE;

	general_params->adwell_default_social_chn =
		IWX_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL;
	general_params->adwell_default_2g = IWX_SCAN_ADWELL_DEFAULT_LB_N_APS;
	general_params->adwell_default_5g = IWX_SCAN_ADWELL_DEFAULT_HB_N_APS;

	if (bgscan)
		general_params->adwell_max_budget =
			htole16(IWX_SCAN_ADWELL_MAX_BUDGET_DIRECTED_SCAN);
	else
		general_params->adwell_max_budget =
			htole16(IWX_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN);

	general_params->scan_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
	if (bgscan) {
		max_out_time = htole32(120);
		suspend_time = htole32(120);
	} else {
		max_out_time = htole32(0);
		suspend_time = htole32(0);
	}
	general_params->max_out_of_time[IWX_SCAN_LB_LMAC_IDX] =
		htole32(max_out_time);
	general_params->suspend_time[IWX_SCAN_LB_LMAC_IDX] =
		htole32(suspend_time);
	general_params->max_out_of_time[IWX_SCAN_HB_LMAC_IDX] =
		htole32(max_out_time);
	general_params->suspend_time[IWX_SCAN_HB_LMAC_IDX] =
		htole32(suspend_time);

	general_params->active_dwell[IWX_SCAN_LB_LMAC_IDX] = active_dwell;
	general_params->passive_dwell[IWX_SCAN_LB_LMAC_IDX] = passive_dwell;
	general_params->active_dwell[IWX_SCAN_HB_LMAC_IDX] = active_dwell;
	general_params->passive_dwell[IWX_SCAN_HB_LMAC_IDX] = passive_dwell;
}

static void
iwx_scan_umac_dwell_v11(struct iwx_softc *sc,
    struct iwx_scan_general_params_v11 *general_params, int bgscan)
{
	uint32_t suspend_time, max_out_time;
	uint8_t active_dwell, passive_dwell;

	active_dwell = IWX_SCAN_DWELL_ACTIVE;
	passive_dwell = IWX_SCAN_DWELL_PASSIVE;

	general_params->adwell_default_social_chn =
		IWX_SCAN_ADWELL_DEFAULT_N_APS_SOCIAL;
	general_params->adwell_default_2g = IWX_SCAN_ADWELL_DEFAULT_LB_N_APS;
	general_params->adwell_default_5g = IWX_SCAN_ADWELL_DEFAULT_HB_N_APS;

	if (bgscan)
		general_params->adwell_max_budget =
			htole16(IWX_SCAN_ADWELL_MAX_BUDGET_DIRECTED_SCAN);
	else
		general_params->adwell_max_budget =
			htole16(IWX_SCAN_ADWELL_MAX_BUDGET_FULL_SCAN);

	general_params->scan_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
	if (bgscan) {
		max_out_time = htole32(120);
		suspend_time = htole32(120);
	} else {
		max_out_time = htole32(0);
		suspend_time = htole32(0);
	}
	general_params->max_out_of_time[IWX_SCAN_LB_LMAC_IDX] =
		htole32(max_out_time);
	general_params->suspend_time[IWX_SCAN_LB_LMAC_IDX] =
		htole32(suspend_time);
	general_params->max_out_of_time[IWX_SCAN_HB_LMAC_IDX] =
		htole32(max_out_time);
	general_params->suspend_time[IWX_SCAN_HB_LMAC_IDX] =
		htole32(suspend_time);

	general_params->active_dwell[IWX_SCAN_LB_LMAC_IDX] = active_dwell;
	general_params->passive_dwell[IWX_SCAN_LB_LMAC_IDX] = passive_dwell;
	general_params->active_dwell[IWX_SCAN_HB_LMAC_IDX] = active_dwell;
	general_params->passive_dwell[IWX_SCAN_HB_LMAC_IDX] = passive_dwell;
}

static void
iwx_scan_umac_fill_general_p_v10(struct iwx_softc *sc,
    struct iwx_scan_general_params_v10 *gp, uint16_t gen_flags, int bgscan)
{
	iwx_scan_umac_dwell_v10(sc, gp, bgscan);

	gp->flags = htole16(gen_flags);

	if (gen_flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FRAGMENTED_LMAC1)
		gp->num_of_fragments[IWX_SCAN_LB_LMAC_IDX] = 3;
	if (gen_flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FRAGMENTED_LMAC2)
		gp->num_of_fragments[IWX_SCAN_HB_LMAC_IDX] = 3;

	gp->scan_start_mac_id = 0;
}

static void
iwx_scan_umac_fill_general_p_v11(struct iwx_softc *sc,
    struct iwx_scan_general_params_v11 *gp, uint16_t gen_flags, int bgscan)
{
	iwx_scan_umac_dwell_v11(sc, gp, bgscan);

	gp->flags = htole16(gen_flags);
	gp->scan_start_mac_or_link_id = 0;

	if (gen_flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FRAGMENTED_LMAC1)
		gp->num_of_fragments[IWX_SCAN_LB_LMAC_IDX] = 3;
	if (gen_flags & IWX_UMAC_SCAN_GEN_FLAGS_V2_FRAGMENTED_LMAC2)
		gp->num_of_fragments[IWX_SCAN_HB_LMAC_IDX] = 3;
}

static void
iwx_scan_umac_fill_ch_p_v6(struct iwx_softc *sc,
    struct iwx_scan_channel_params_v6 *cp, uint32_t channel_cfg_flags,
    int n_ssid)
{
	cp->flags = IWX_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;

	cp->count = iwx_umac_scan_fill_channels(sc, cp->channel_config,
	    nitems(cp->channel_config), n_ssid, channel_cfg_flags);

	cp->n_aps_override[0] = IWX_SCAN_ADWELL_N_APS_GO_FRIENDLY;
	cp->n_aps_override[1] = IWX_SCAN_ADWELL_N_APS_SOCIAL_CHS;
}

static void
iwx_scan_umac_fill_ch_p_v7(struct iwx_softc *sc,
    struct iwx_scan_channel_params_v7 *cp, uint32_t channel_cfg_flags,
    int n_ssid)
{
	cp->flags = IWX_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;

	cp->count = iwx_umac_scan_fill_channels_v5(sc, cp->channel_config,
	    nitems(cp->channel_config), n_ssid, channel_cfg_flags);

	cp->n_aps_override[0] = IWX_SCAN_ADWELL_N_APS_GO_FRIENDLY;
	cp->n_aps_override[1] = IWX_SCAN_ADWELL_N_APS_SOCIAL_CHS;
}

static int
iwx_umac_scan_v14(struct iwx_softc *sc, int bgscan)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_host_cmd hcmd = {
		.id = iwx_cmd_id(IWX_SCAN_REQ_UMAC, IWX_LONG_GROUP, 0),
		.len = { 0, },
		.data = { NULL, },
		.flags = 0,
	};
	struct iwx_scan_req_umac_v14 *cmd;
	struct iwx_scan_req_params_v14 *scan_p;
	int err, async = bgscan, n_ssid = 0;
	uint16_t gen_flags;
	uint32_t bitmap_ssid = 0;

	cmd = kmem_intr_zalloc(sizeof(*cmd), KM_NOSLEEP);
	if (cmd == NULL)
		return ENOMEM;

	scan_p = &cmd->scan_params;

	cmd->ooc_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
	cmd->uid = htole32(0);

	gen_flags = iwx_scan_umac_flags_v2(sc, bgscan);
	iwx_scan_umac_fill_general_p_v10(sc, &scan_p->general_params,
	    gen_flags, bgscan);

	scan_p->periodic_params.schedule[0].interval = htole16(0);
	scan_p->periodic_params.schedule[0].iter_count = 1;

	err = iwx_fill_probe_req(sc, &scan_p->probe_params.preq);
	if (err) {
		kmem_intr_free(cmd, sizeof(*cmd));
		return err;
	}

	if (ic->ic_des_esslen != 0) {
		scan_p->probe_params.direct_scan[0].id = IEEE80211_ELEMID_SSID;
		scan_p->probe_params.direct_scan[0].len = ic->ic_des_esslen;
		memcpy(scan_p->probe_params.direct_scan[0].ssid,
		    ic->ic_des_essid, ic->ic_des_esslen);
		bitmap_ssid |= (1 << 0);
		n_ssid = 1;
	}

	iwx_scan_umac_fill_ch_p_v6(sc, &scan_p->channel_params, bitmap_ssid,
	    n_ssid);

	hcmd.len[0] = sizeof(*cmd);
	hcmd.data[0] = (void *)cmd;
	hcmd.flags |= async ? IWX_CMD_ASYNC : 0;

	err = iwx_send_cmd(sc, &hcmd);
	kmem_intr_free(cmd, sizeof(*cmd));
	return err;
}

static int
iwx_umac_scan_v17(struct iwx_softc *sc, int bgscan)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_host_cmd hcmd = {
		.id = iwx_cmd_id(IWX_SCAN_REQ_UMAC, IWX_LONG_GROUP, 0),
		.len = { 0, },
		.data = { NULL, },
		.flags = 0,
	};
	struct iwx_scan_req_umac_v17 *cmd;
	struct iwx_scan_req_params_v17 *scan_p;
	int err, async = bgscan, n_ssid = 0;
	uint16_t gen_flags;
	uint32_t bitmap_ssid = 0;

	cmd = kmem_intr_zalloc(sizeof(*cmd), KM_NOSLEEP);
	if (cmd == NULL)
		return ENOMEM;

	scan_p = &cmd->scan_params;

	cmd->ooc_priority = htole32(IWX_SCAN_PRIORITY_EXT_6);
	cmd->uid = htole32(0);

	gen_flags = iwx_scan_umac_flags_v2(sc, bgscan);
	iwx_scan_umac_fill_general_p_v11(sc, &scan_p->general_params,
	    gen_flags, bgscan);

	scan_p->periodic_params.schedule[0].interval = htole16(0);
	scan_p->periodic_params.schedule[0].iter_count = 1;

	err = iwx_fill_probe_req(sc, &scan_p->probe_params.preq);
	if (err) {
		kmem_intr_free(cmd, sizeof(*cmd));
		return err;
	}

	if (ic->ic_des_esslen != 0) {
		scan_p->probe_params.direct_scan[0].id = IEEE80211_ELEMID_SSID;
		scan_p->probe_params.direct_scan[0].len = ic->ic_des_esslen;
		memcpy(scan_p->probe_params.direct_scan[0].ssid,
		    ic->ic_des_essid, ic->ic_des_esslen);
		bitmap_ssid |= (1 << 0);
		n_ssid = 1;
	}

	iwx_scan_umac_fill_ch_p_v7(sc, &scan_p->channel_params, bitmap_ssid,
	    n_ssid);

	hcmd.len[0] = sizeof(*cmd);
	hcmd.data[0] = (void *)cmd;
	hcmd.flags |= async ? IWX_CMD_ASYNC : 0;

	err = iwx_send_cmd(sc, &hcmd);
	kmem_intr_free(cmd, sizeof(*cmd));
	return err;
}

static void
iwx_mcc_update(struct iwx_softc *sc, struct iwx_mcc_chub_notif *notif)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	char alpha2[3];

	snprintf(alpha2, sizeof(alpha2), "%c%c",
	    (le16toh(notif->mcc) & 0xff00) >> 8, le16toh(notif->mcc) & 0xff);

	if (ifp->if_flags & IFF_DEBUG) {
		printf("%s: firmware has detected regulatory domain '%s' "
		    "(0x%x)\n", DEVNAME(sc), alpha2, le16toh(notif->mcc));
	}

	/* TODO: Schedule a task to send MCC_UPDATE_CMD? */
}

static uint8_t
iwx_ridx2rate(struct ieee80211_rateset *rs, int ridx)
{
	int i;
	uint8_t rval;

	for (i = 0; i < rs->rs_nrates; i++) {
		rval = (rs->rs_rates[i] & IEEE80211_RATE_VAL);
		if (rval == iwx_rates[ridx].rate)
			return rs->rs_rates[i];
	}

	return 0;
}

static void
iwx_ack_rates(struct iwx_softc *sc, struct iwx_node *in, int *cck_rates,
    int *ofdm_rates)
{
	struct ieee80211_node *ni = &in->in_ni;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	int lowest_present_ofdm = -1;
	int lowest_present_cck = -1;
	uint8_t cck = 0;
	uint8_t ofdm = 0;
	int i;

	if (ni->ni_chan == IEEE80211_CHAN_ANYC ||
	    IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
		for (i = IWX_FIRST_CCK_RATE; i < IWX_FIRST_OFDM_RATE; i++) {
			if ((iwx_ridx2rate(rs, i) & IEEE80211_RATE_BASIC) == 0)
				continue;
			cck |= (1 << i);
			if (lowest_present_cck == -1 || lowest_present_cck > i)
				lowest_present_cck = i;
		}
	}
	for (i = IWX_FIRST_OFDM_RATE; i <= IWX_LAST_NON_HT_RATE; i++) {
		if ((iwx_ridx2rate(rs, i) & IEEE80211_RATE_BASIC) == 0)
			continue;
		ofdm |= (1 << (i - IWX_FIRST_OFDM_RATE));
		if (lowest_present_ofdm == -1 || lowest_present_ofdm > i)
			lowest_present_ofdm = i;
	}

	/*
	 * Now we've got the basic rates as bitmaps in the ofdm and cck
	 * variables. This isn't sufficient though, as there might not
	 * be all the right rates in the bitmap. E.g. if the only basic
	 * rates are 5.5 Mbps and 11 Mbps, we still need to add 1 Mbps
	 * and 6 Mbps because the 802.11-2007 standard says in 9.6:
	 *
	 *    [...] a STA responding to a received frame shall transmit
	 *    its Control Response frame [...] at the highest rate in the
	 *    BSSBasicRateSet parameter that is less than or equal to the
	 *    rate of the immediately previous frame in the frame exchange
	 *    sequence ([...]) and that is of the same modulation class
	 *    ([...]) as the received frame. If no rate contained in the
	 *    BSSBasicRateSet parameter meets these conditions, then the
	 *    control frame sent in response to a received frame shall be
	 *    transmitted at the highest mandatory rate of the PHY that is
	 *    less than or equal to the rate of the received frame, and
	 *    that is of the same modulation class as the received frame.
	 *
	 * As a consequence, we need to add all mandatory rates that are
	 * lower than all of the basic rates to these bitmaps.
	 */

	if (IWX_RATE_24M_INDEX < lowest_present_ofdm)
		ofdm |= IWX_RATE_BIT_MSK(24) >> IWX_FIRST_OFDM_RATE;
	if (IWX_RATE_12M_INDEX < lowest_present_ofdm)
		ofdm |= IWX_RATE_BIT_MSK(12) >> IWX_FIRST_OFDM_RATE;
	/* 6M already there or needed so always add */
	ofdm |= IWX_RATE_BIT_MSK(6) >> IWX_FIRST_OFDM_RATE;

	/*
	 * CCK is a bit more complex with DSSS vs. HR/DSSS vs. ERP.
	 * Note, however:
	 *  - if no CCK rates are basic, it must be ERP since there must
	 *    be some basic rates at all, so they're OFDM => ERP PHY
	 *    (or we're in 5 GHz, and the cck bitmap will never be used)
	 *  - if 11M is a basic rate, it must be ERP as well, so add 5.5M
	 *  - if 5.5M is basic, 1M and 2M are mandatory
	 *  - if 2M is basic, 1M is mandatory
	 *  - if 1M is basic, that's the only valid ACK rate.
	 * As a consequence, it's not as complicated as it sounds, just add
	 * any lower rates to the ACK rate bitmap.
	 */
	if (IWX_RATE_11M_INDEX < lowest_present_cck)
		cck |= IWX_RATE_BIT_MSK(11) >> IWX_FIRST_CCK_RATE;
	if (IWX_RATE_5M_INDEX < lowest_present_cck)
		cck |= IWX_RATE_BIT_MSK(5) >> IWX_FIRST_CCK_RATE;
	if (IWX_RATE_2M_INDEX < lowest_present_cck)
		cck |= IWX_RATE_BIT_MSK(2) >> IWX_FIRST_CCK_RATE;
	/* 1M already there or needed so always add */
	cck |= IWX_RATE_BIT_MSK(1) >> IWX_FIRST_CCK_RATE;

	*cck_rates = cck;
	*ofdm_rates = ofdm;
}

static void
iwx_mac_ctxt_cmd_common(struct iwx_softc *sc, struct iwx_node *in,
    struct iwx_mac_ctx_cmd *cmd, uint32_t action)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int cck_ack_rates, ofdm_ack_rates;
	int i;

	cmd->id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(in->in_id,
	    in->in_color));
	cmd->action = htole32(action);

	if (action == IWX_FW_CTXT_ACTION_REMOVE)
		return;

	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		cmd->mac_type = htole32(IWX_FW_MAC_TYPE_LISTENER);
	else if (ic->ic_opmode == IEEE80211_M_STA)
		cmd->mac_type = htole32(IWX_FW_MAC_TYPE_BSS_STA);
	else
		panic("unsupported operating mode %d", ic->ic_opmode);
	cmd->tsf_id = htole32(IWX_TSF_ID_A);

	IEEE80211_ADDR_COPY(cmd->node_addr, ic->ic_myaddr);
	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		IEEE80211_ADDR_COPY(cmd->bssid_addr, etherbroadcastaddr);
		return;
	}

	IEEE80211_ADDR_COPY(cmd->bssid_addr, in->in_macaddr);
	iwx_ack_rates(sc, in, &cck_ack_rates, &ofdm_ack_rates);
	cmd->cck_rates = htole32(cck_ack_rates);
	cmd->ofdm_rates = htole32(ofdm_ack_rates);

	cmd->cck_short_preamble
	    = htole32((ic->ic_flags & IEEE80211_F_SHPREAMBLE)
	      ? IWX_MAC_FLG_SHORT_PREAMBLE : 0);
	cmd->short_slot
	    = htole32((ic->ic_flags & IEEE80211_F_SHSLOT)
	      ? IWX_MAC_FLG_SHORT_SLOT : 0);

	for (i = 0; i < WME_NUM_AC; i++) {
		uint16_t cw_min, cw_max, txop;
		uint8_t aifsn;
		int txf;

		if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
			txf = iwx_ac_to_bz_tx_fifo[i];
		else
			txf = iwx_ac_to_tx_fifo[i];

		iwx_get_edca_params(sc, i, &cw_min, &cw_max, &aifsn, &txop);
		cmd->ac[txf].cw_min = htole16(cw_min);
		cmd->ac[txf].cw_max = htole16(cw_max);
		cmd->ac[txf].aifsn = aifsn;
		cmd->ac[txf].fifos_mask = (1 << txf);
		cmd->ac[txf].edca_txop = htole16(txop * 32);
	}
	if (ni->ni_flags & IEEE80211_NODE_QOS)
		cmd->qos_flags |= htole32(IWX_MAC_QOS_FLG_UPDATE_EDCA);

	if (ic->ic_flags & IEEE80211_F_USEPROT)
		cmd->protection_flags |= htole32(IWX_MAC_PROT_FLG_TGG_PROTECT);

	cmd->filter_flags = htole32(IWX_MAC_FILTER_ACCEPT_GRP);
}

static void
iwx_mac_ctxt_cmd_fill_sta(struct iwx_softc *sc, struct iwx_node *in,
    struct iwx_mac_data_sta *sta, int assoc)
{
	struct ieee80211_node *ni = &in->in_ni;
	uint32_t dtim_off;
	uint64_t tsf;

	dtim_off = ni->ni_dtim_count * ni->ni_intval * IEEE80211_DUR_TU;
	memcpy(&tsf, ni->ni_tstamp.data, sizeof(tsf));
	tsf = letoh64(tsf);

	sta->is_assoc = htole32(assoc);
	if (assoc) {
		sta->dtim_time = htole32(ni->ni_rstamp + dtim_off);
		sta->dtim_tsf = htole64(tsf + dtim_off);
		sta->assoc_beacon_arrive_time = htole32(ni->ni_rstamp);
	}
	sta->bi = htole32(ni->ni_intval);
	sta->dtim_interval = htole32(ni->ni_intval * ni->ni_dtim_period);
	sta->data_policy = htole32(0);
	sta->listen_interval = htole32(10);
	sta->assoc_id = htole32(IEEE80211_AID(ni->ni_associd));
}

static int
iwx_mac_ctxt_cmd(struct iwx_softc *sc, struct iwx_node *in, uint32_t action,
    int assoc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = &in->in_ni;
	struct iwx_mac_ctx_cmd cmd;
	int active = (sc->sc_flags & IWX_FLAG_MAC_ACTIVE);

	if (action == IWX_FW_CTXT_ACTION_ADD && active)
		panic("MAC already added");
	if (action == IWX_FW_CTXT_ACTION_REMOVE && !active)
		panic("MAC already removed");

	if (sc->sc_use_mld_api)
		return iwx_mld_mac_ctxt_cmd(sc, in, action, assoc);

	memset(&cmd, 0, sizeof(cmd));

	iwx_mac_ctxt_cmd_common(sc, in, &cmd, action);

	if (action == IWX_FW_CTXT_ACTION_REMOVE) {
		return iwx_send_cmd_pdu(sc, IWX_MAC_CONTEXT_CMD, 0,
		    sizeof(cmd), &cmd);
	}

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		cmd.filter_flags |= htole32(IWX_MAC_FILTER_IN_PROMISC |
		    IWX_MAC_FILTER_IN_CONTROL_AND_MGMT |
		    IWX_MAC_FILTER_ACCEPT_GRP |
		    IWX_MAC_FILTER_IN_BEACON |
		    IWX_MAC_FILTER_IN_PROBE_REQUEST |
		    IWX_MAC_FILTER_IN_CRC32);
	} else if (!assoc || !ni->ni_associd || !ni->ni_dtim_period) {
		/*
		 * Allow beacons to pass through as long as we are not
		 * associated or we do not have dtim period information.
		 */
		cmd.filter_flags |= htole32(IWX_MAC_FILTER_IN_BEACON);
	}
	iwx_mac_ctxt_cmd_fill_sta(sc, in, &cmd.sta, assoc);
	return iwx_send_cmd_pdu(sc, IWX_MAC_CONTEXT_CMD, 0, sizeof(cmd), &cmd);
}

static int
iwx_mld_mac_ctxt_cmd(struct iwx_softc *sc, struct iwx_node *in,
    uint32_t action, int assoc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = &in->in_ni;
	struct iwx_mac_config_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id_and_color = htole32(in->in_id);
	cmd.action = htole32(action);

	if (action == IWX_FW_CTXT_ACTION_REMOVE) {
		return iwx_send_cmd_pdu(sc,
		    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD),
		    0, sizeof(cmd), &cmd);
	}

	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		cmd.mac_type = htole32(IWX_FW_MAC_TYPE_LISTENER);
	else if (ic->ic_opmode == IEEE80211_M_STA)
		cmd.mac_type = htole32(IWX_FW_MAC_TYPE_BSS_STA);
	else
		panic("unsupported operating mode %d", ic->ic_opmode);
	IEEE80211_ADDR_COPY(cmd.local_mld_addr, ic->ic_myaddr);
	cmd.client.assoc_id = htole32(IEEE80211_AID(ni->ni_associd));
	cmd.client.is_assoc = assoc ? 1 : 0;

	cmd.filter_flags = htole32(IWX_MAC_CFG_FILTER_ACCEPT_GRP);
	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		cmd.filter_flags |= htole32(IWX_MAC_CFG_FILTER_PROMISC |
		    IWX_MAC_FILTER_IN_CONTROL_AND_MGMT |
		    IWX_MAC_CFG_FILTER_ACCEPT_BEACON |
		    IWX_MAC_CFG_FILTER_ACCEPT_PROBE_REQ |
		    IWX_MAC_CFG_FILTER_ACCEPT_GRP);
	} else if (!assoc || !ni->ni_associd || !ni->ni_dtim_period) {
		/*
		 * Allow beacons to pass through as long as we are not
		 * associated or we do not have dtim period information.
		 */
		cmd.filter_flags |= htole32(IWX_MAC_CFG_FILTER_ACCEPT_BEACON);
	}

	return iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD),
	    0, sizeof(cmd), &cmd);
}

static int
iwx_send_statistics_cmd_clear(struct iwx_softc *sc)
{
	struct iwx_statistics_cmd scmd = {
		.flags = htole32(IWX_STATISTICS_FLG_CLEAR)
	};
	struct iwx_host_cmd cmd = {
		.id = IWX_STATISTICS_CMD,
		.len[0] = sizeof(scmd),
		.data[0] = &scmd,
		.flags = IWX_CMD_WANT_RESP,
		.resp_pkt_len = sizeof(struct iwx_notif_statistics),
	};
	int err;

	err = iwx_send_cmd(sc, &cmd);
	if (err)
		return err;

	iwx_free_resp(sc, &cmd);
	return 0;
}

static int
iwx_send_system_statistics_cmd_clear(struct iwx_softc *sc)
{
	struct iwx_system_statistics_cmd scmd = {
		.cfg_mask = htole32(IWX_STATS_CFG_FLG_ON_DEMAND_NTFY_MSK),
		.type_id_mask = htole32(IWX_STATS_NTFY_TYPE_ID_OPER |
		    IWX_STATS_NTFY_TYPE_ID_OPER_PART1),
	};
	struct iwx_host_cmd cmd = {
		.id = IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_SYSTEM_STATISTICS_CMD),
		.len[0] = sizeof(scmd),
		.data[0] = &scmd,
		.flags = IWX_CMD_ASYNC,
	};
	int err;

	sc->sc_system_stats_cleared = 0;

	err = iwx_send_cmd(sc, &cmd);
	if (err)
		return err;

	/* Wait for SYSTEM_STATISTICS_END_NOTIF firmware notification. */
	while (!sc->sc_system_stats_cleared) {
		err = tsleep(&sc->sc_system_stats_cleared, 0, "iwxstat",
		    SEC_TO_TICKS(1));
		if (err)
			break;
	}

	return err;
}

static int
iwx_clear_statistics(struct iwx_softc *sc)
{
	uint32_t ver;

	ver = iwx_lookup_cmd_ver(sc, IWX_SYSTEM_GROUP,
	    IWX_SYSTEM_STATISTICS_CMD);

	switch (ver) {
	case IWX_FW_CMD_VER_UNKNOWN:
		return iwx_send_statistics_cmd_clear(sc);
	case 1:
		return iwx_send_system_statistics_cmd_clear(sc);
	default:
		printf("%s: unknown statistics command version %d\n",
		    DEVNAME(sc), ver);
		break;
	}

	return 0;
}

static void
iwx_add_task(struct iwx_softc *sc, struct iwx_taskq *taskq,
    struct iwx_task *task)
{
	int s = splnet();

	if (sc->sc_flags & IWX_FLAG_SHUTDOWN) {
		splx(s);
		return;
	}

	iwx_refcnt_take(sc);
	if (!iwx_task_add(taskq, task))
		iwx_refcnt_rele_wake(sc);
	splx(s);
}

static void
iwx_del_task(struct iwx_softc *sc, struct iwx_taskq *taskq,
    struct iwx_task *task)
{
	if (iwx_task_del(taskq, task))
		iwx_refcnt_rele_wake(sc);
}

static int
iwx_initiate_scan(struct iwx_softc *sc, int bgscan)
{
	int err;
	uint8_t ver;

	ver = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC);
	DPRINTF(("%s: scan command version %u\n", DEVNAME(sc), ver));

	switch (ver) {
	case 17:
		err = iwx_umac_scan_v17(sc, bgscan);
		break;
	default:
		err = iwx_umac_scan_v14(sc, bgscan);
		break;
	}

	return err;
}

/*
 * Start a firmware scan. This replaces net80211's per-channel scanning:
 * the device scans all channels at once and we switch to SCAN state
 * ourselves instead of calling the generic state machine, taking care
 * of the bookkeeping that ieee80211_newstate() would otherwise do.
 */
static int
iwx_scan(struct iwx_softc *sc, enum ieee80211_state ostate)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	int err;

	if (sc->sc_flags & IWX_FLAG_BGSCAN) {
		err = iwx_scan_abort(sc);
		if (err) {
			printf("%s: could not abort background scan\n",
			    DEVNAME(sc));
			return err;
		}
	}

	err = iwx_initiate_scan(sc, 0);
	if (err) {
		printf("%s: could not initiate scan\n", DEVNAME(sc));
		return err;
	}

	/*
	 * The current mode might have been fixed during association.
	 * Ensure all channels get scanned.
	 */
	if (IFM_MODE(ic->ic_media.ifm_cur->ifm_media) == IFM_AUTO &&
	    ic->ic_curmode != IEEE80211_MODE_AUTO)
		ieee80211_setmode(ic, IEEE80211_MODE_AUTO);

	sc->sc_flags |= IWX_FLAG_SCANNING;
	if (ifp->if_flags & IFF_DEBUG)
		printf("%s: %s -> %s\n", ifp->if_xname,
		    ieee80211_state_name[ic->ic_state],
		    ieee80211_state_name[IEEE80211_S_SCAN]);
	if ((sc->sc_flags & IWX_FLAG_BGSCAN) == 0) {
		if (ostate == IEEE80211_S_RUN) {
			/* Beacon miss or deauth; leave the BSS. */
			ieee80211_sta_leave(ic, ic->ic_bss);
			ic->ic_flags &= ~IEEE80211_F_SIBSS;
		} else if (ostate == IEEE80211_S_AUTH ||
		    ostate == IEEE80211_S_ASSOC) {
			struct ieee80211_node *ni;
			ni = ieee80211_find_node(&ic->ic_scan,
			    ic->ic_bss->ni_macaddr);
			if (ni != NULL) {
				ni->ni_fails++;
				ieee80211_free_node(ni);
			}
		}
		ic->ic_mgt_timer = 0;
		ic->ic_flags |= IEEE80211_F_SCAN | IEEE80211_F_ASCAN;
	}
	ic->ic_state = IEEE80211_S_SCAN;
	wakeup(&ic->ic_state); /* wake iwx_init() */

	return 0;
}

static int
iwx_umac_scan_abort(struct iwx_softc *sc)
{
	struct iwx_umac_scan_abort cmd = { 0 };

	return iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_LONG_GROUP, IWX_SCAN_ABORT_UMAC),
	    0, sizeof(cmd), &cmd);
}

static int
iwx_scan_abort(struct iwx_softc *sc)
{
	int err;

	err = iwx_umac_scan_abort(sc);
	if (err == 0)
		sc->sc_flags &= ~(IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN);
	return err;
}

static int
iwx_enable_mgmt_queue(struct iwx_softc *sc)
{
	int err;

	sc->first_data_qid = IWX_DQA_CMD_QUEUE + 1;

	/*
	 * Non-QoS frames use the "MGMT" TID and queue.
	 * Other TIDs and data queues are reserved for QoS data frames.
	 */
	err = iwx_enable_txq(sc, IWX_STATION_ID, sc->first_data_qid,
	    IWX_MGMT_TID, IWX_TX_RING_COUNT);
	if (err) {
		printf("%s: could not enable Tx queue %d (error %d)\n",
		    DEVNAME(sc), sc->first_data_qid, err);
		return err;
	}

	return 0;
}

static int
iwx_disable_mgmt_queue(struct iwx_softc *sc)
{
	int err, cmd_ver;

	/* Explicit removal is only required with old SCD_QUEUE_CFG command. */
	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_SCD_QUEUE_CONFIG_CMD);
	if (cmd_ver == 0 || cmd_ver == IWX_FW_CMD_VER_UNKNOWN)
		return 0;

	sc->first_data_qid = IWX_DQA_CMD_QUEUE + 1;

	err = iwx_disable_txq(sc, IWX_STATION_ID, sc->first_data_qid,
	    IWX_MGMT_TID);
	if (err) {
		printf("%s: could not disable Tx queue %d (error %d)\n",
		    DEVNAME(sc), sc->first_data_qid, err);
		return err;
	}

	return 0;
}

static int
iwx_rs_rval2idx(uint8_t rval)
{
	/* Firmware expects indices which match our 11g rate set. */
	const struct ieee80211_rateset *rs = &ieee80211_std_rateset_11g;
	int i;

	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_VAL) == rval)
			return i;
	}

	return -1;
}

static int
iwx_rs_init_v3(struct iwx_softc *sc, struct iwx_node *in)
{
	struct ieee80211_node *ni = &in->in_ni;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	struct iwx_tlc_config_cmd_v3 cfg_cmd;
	uint32_t cmd_id;
	int i;
	size_t cmd_size = sizeof(cfg_cmd);

	memset(&cfg_cmd, 0, sizeof(cfg_cmd));

	for (i = 0; i < rs->rs_nrates; i++) {
		uint8_t rval = rs->rs_rates[i] & IEEE80211_RATE_VAL;
		int idx = iwx_rs_rval2idx(rval);
		if (idx == -1)
			return EINVAL;
		cfg_cmd.non_ht_rates |= (1 << idx);
	}

	cfg_cmd.mode = IWX_TLC_MNG_MODE_NON_HT;
	cfg_cmd.sta_id = IWX_STATION_ID;
	cfg_cmd.max_ch_width = IWX_TLC_MNG_CH_WIDTH_20MHZ;
	cfg_cmd.chains = IWX_TLC_MNG_CHAIN_A_MSK;
	cfg_cmd.max_mpdu_len = htole16(IEEE80211_MAX_LEN);

	cmd_id = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
	return iwx_send_cmd_pdu(sc, cmd_id, IWX_CMD_ASYNC, cmd_size, &cfg_cmd);
}

static int
iwx_rs_init_v4(struct iwx_softc *sc, struct iwx_node *in)
{
	struct ieee80211_node *ni = &in->in_ni;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	struct iwx_tlc_config_cmd_v4 cfg_cmd;
	uint32_t cmd_id;
	int i;
	size_t cmd_size = sizeof(cfg_cmd);

	memset(&cfg_cmd, 0, sizeof(cfg_cmd));

	for (i = 0; i < rs->rs_nrates; i++) {
		uint8_t rval = rs->rs_rates[i] & IEEE80211_RATE_VAL;
		int idx = iwx_rs_rval2idx(rval);
		if (idx == -1)
			return EINVAL;
		cfg_cmd.non_ht_rates |= (1 << idx);
	}

	cfg_cmd.mode = IWX_TLC_MNG_MODE_NON_HT;
	cfg_cmd.sta_id = IWX_STATION_ID;
	cfg_cmd.max_ch_width = IWX_TLC_MNG_CH_WIDTH_20MHZ;
	cfg_cmd.chains = IWX_TLC_MNG_CHAIN_A_MSK;
	cfg_cmd.max_mpdu_len = htole16(IEEE80211_MAX_LEN);

	cmd_id = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
	return iwx_send_cmd_pdu(sc, cmd_id, IWX_CMD_ASYNC, cmd_size, &cfg_cmd);
}

static int
iwx_rs_init(struct iwx_softc *sc, struct iwx_node *in)
{
	int cmd_ver;

	cmd_ver = iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_TLC_MNG_CONFIG_CMD);
	if (cmd_ver == 4)
		return iwx_rs_init_v4(sc, in);
	return iwx_rs_init_v3(sc, in);
}

static void
iwx_rs_update(struct iwx_softc *sc, struct iwx_tlc_update_notif *notif)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct ieee80211_rateset *rs = &ni->ni_rates;
	uint32_t rate_n_flags;
	uint8_t plcp, rval;
	int i, cmd_ver, rate_n_flags_ver2 = 0;

	if (notif->sta_id != IWX_STATION_ID ||
	    (le32toh(notif->flags) & IWX_TLC_NOTIF_FLAG_RATE) == 0)
		return;

	rate_n_flags = le32toh(notif->rate);

	cmd_ver = iwx_lookup_notif_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_TLC_MNG_UPDATE_NOTIF);
	if (cmd_ver != IWX_FW_CMD_VER_UNKNOWN && cmd_ver >= 3)
		rate_n_flags_ver2 = 1;
	if (rate_n_flags_ver2) {
		uint32_t mod_type = (rate_n_flags & IWX_RATE_MCS_MOD_TYPE_MSK);
		if (mod_type == IWX_RATE_MCS_VHT_MSK ||
		    mod_type == IWX_RATE_MCS_HT_MSK)
			return; /* HT/VHT not supported */
	} else {
		if (rate_n_flags & (IWX_RATE_MCS_VHT_MSK_V1 |
		    IWX_RATE_MCS_HT_MSK_V1))
			return; /* HT/VHT not supported */
	}

	if (rate_n_flags_ver2) {
		const struct ieee80211_rateset *srs;
		uint32_t ridx = (rate_n_flags & IWX_RATE_LEGACY_RATE_MSK);
		if (rate_n_flags & IWX_RATE_MCS_LEGACY_OFDM_MSK)
			srs = &ieee80211_std_rateset_11a;
		else
			srs = &ieee80211_std_rateset_11b;
		if (ridx < srs->rs_nrates)
			rval = (srs->rs_rates[ridx] & IEEE80211_RATE_VAL);
		else
			rval = 0;
	} else {
		plcp = (rate_n_flags & IWX_RATE_LEGACY_RATE_MSK_V1);

		rval = 0;
		for (i = IWX_RATE_1M_INDEX; i < nitems(iwx_rates); i++) {
			if (iwx_rates[i].plcp == plcp) {
				rval = iwx_rates[i].rate;
				break;
			}
		}
	}

	if (rval) {
		uint8_t rv;
		for (i = 0; i < rs->rs_nrates; i++) {
			rv = rs->rs_rates[i] & IEEE80211_RATE_VAL;
			if (rv == rval) {
				ni->ni_txrate = i;
				break;
			}
		}
	}
}

static int
iwx_phy_send_rlc(struct iwx_softc *sc, struct iwx_phy_ctxt *phyctxt,
    uint8_t chains_static, uint8_t chains_dynamic)
{
	struct iwx_rlc_config_cmd cmd;
	uint32_t cmd_id;
	uint8_t active_cnt, idle_cnt;

	memset(&cmd, 0, sizeof(cmd));

	idle_cnt = chains_static;
	active_cnt = chains_dynamic;

	cmd.phy_id = htole32(phyctxt->id);
	cmd.rlc.rx_chain_info = htole32(iwx_fw_valid_rx_ant(sc) <<
	    IWX_PHY_RX_CHAIN_VALID_POS);
	cmd.rlc.rx_chain_info |= htole32(idle_cnt << IWX_PHY_RX_CHAIN_CNT_POS);
	cmd.rlc.rx_chain_info |= htole32(active_cnt <<
	    IWX_PHY_RX_CHAIN_MIMO_CNT_POS);

	cmd_id = iwx_cmd_id(IWX_RLC_CONFIG_CMD, IWX_DATA_PATH_GROUP, 2);
	return iwx_send_cmd_pdu(sc, cmd_id, 0, sizeof(cmd), &cmd);
}

static int
iwx_auth(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	uint32_t duration;
	int generation = sc->sc_generation, err;

	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		sc->sc_phyctxt[0].channel = ic->ic_ibss_chan;
	else
		sc->sc_phyctxt[0].channel = in->in_ni.ni_chan;

	if (sc->sc_phyctxt[0].channel == NULL ||
	    sc->sc_phyctxt[0].channel == IEEE80211_CHAN_ANYC) {
		printf("%s: no channel for BSS\n", DEVNAME(sc));
		return EINVAL;
	}

	err = iwx_phy_ctxt_cmd(sc, &sc->sc_phyctxt[0], 1, 1,
	    IWX_FW_CTXT_ACTION_ADD, 0, 0, 0);
	if (err) {
		printf("%s: could not add phy context (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}
	sc->sc_flags |= IWX_FLAG_PHY_ACTIVE;

	if (iwx_lookup_cmd_ver(sc, IWX_DATA_PATH_GROUP,
	    IWX_RLC_CONFIG_CMD) == 2) {
		err = iwx_phy_send_rlc(sc, &sc->sc_phyctxt[0], 1, 1);
		if (err) {
			printf("%s: could not configure RLC for PHY "
			    "(error %d)\n", DEVNAME(sc), err);
			goto rm_phy_ctxt;
		}
	}

	in->in_phyctxt = &sc->sc_phyctxt[0];
	IEEE80211_ADDR_COPY(in->in_macaddr, in->in_ni.ni_macaddr);

	err = iwx_mac_ctxt_cmd(sc, in, IWX_FW_CTXT_ACTION_ADD, 0);
	if (err) {
		printf("%s: could not add MAC context (error %d)\n",
		    DEVNAME(sc), err);
		goto rm_phy_ctxt;
 	}
	sc->sc_flags |= IWX_FLAG_MAC_ACTIVE;

	err = iwx_binding_cmd(sc, in, IWX_FW_CTXT_ACTION_ADD);
	if (err) {
		printf("%s: could not add binding (error %d)\n",
		    DEVNAME(sc), err);
		goto rm_mac_ctxt;
	}
	sc->sc_flags |= IWX_FLAG_BINDING_ACTIVE;

	err = iwx_add_sta_cmd(sc, in, 0);
	if (err) {
		printf("%s: could not add sta (error %d)\n",
		    DEVNAME(sc), err);
		goto rm_binding;
	}
	sc->sc_flags |= IWX_FLAG_STA_ACTIVE;

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		err = iwx_enable_txq(sc, IWX_MONITOR_STA_ID,
		    IWX_DQA_INJECT_MONITOR_QUEUE, IWX_MGMT_TID,
		    IWX_TX_RING_COUNT);
		if (err)
			goto rm_sta;
		return 0;
	}

	err = iwx_enable_mgmt_queue(sc);
	if (err)
		goto rm_sta;

	err = iwx_clear_statistics(sc);
	if (err)
		goto rm_mgmt_queue;

	/*
	 * Prevent the FW from wandering off channel during association
	 * by "protecting" the session with a time event.
	 */
	if (in->in_ni.ni_intval)
		duration = in->in_ni.ni_intval * 9;
	else
		duration = 900;
	return iwx_schedule_session_protection(sc, in, duration);
rm_mgmt_queue:
	if (generation == sc->sc_generation)
		iwx_disable_mgmt_queue(sc);
rm_sta:
	if (generation == sc->sc_generation) {
		iwx_rm_sta_cmd(sc, in);
		sc->sc_flags &= ~IWX_FLAG_STA_ACTIVE;
	}
rm_binding:
	if (generation == sc->sc_generation) {
		iwx_binding_cmd(sc, in, IWX_FW_CTXT_ACTION_REMOVE);
		sc->sc_flags &= ~IWX_FLAG_BINDING_ACTIVE;
	}
rm_mac_ctxt:
	if (generation == sc->sc_generation) {
		iwx_mac_ctxt_cmd(sc, in, IWX_FW_CTXT_ACTION_REMOVE, 0);
		sc->sc_flags &= ~IWX_FLAG_MAC_ACTIVE;
	}
rm_phy_ctxt:
	if (generation == sc->sc_generation) {
		iwx_phy_ctxt_cmd(sc, &sc->sc_phyctxt[0], 1, 1,
		    IWX_FW_CTXT_ACTION_REMOVE, 0, 0, 0);
		sc->sc_flags &= ~IWX_FLAG_PHY_ACTIVE;
	}
	return err;
}

static int
iwx_deauth(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	int err;

	iwx_unprotect_session(sc, in);

	if (sc->sc_flags & IWX_FLAG_STA_ACTIVE) {
		err = iwx_rm_sta(sc, in);
		if (err)
			return err;
		sc->sc_flags &= ~IWX_FLAG_STA_ACTIVE;
	}

	if (sc->sc_flags & IWX_FLAG_BINDING_ACTIVE) {
		err = iwx_binding_cmd(sc, in, IWX_FW_CTXT_ACTION_REMOVE);
		if (err) {
			printf("%s: could not remove binding (error %d)\n",
			    DEVNAME(sc), err);
			return err;
		}
		sc->sc_flags &= ~IWX_FLAG_BINDING_ACTIVE;
	}

	if (sc->sc_flags & IWX_FLAG_MAC_ACTIVE) {
		err = iwx_mac_ctxt_cmd(sc, in, IWX_FW_CTXT_ACTION_REMOVE, 0);
		if (err) {
			printf("%s: could not remove MAC context (error %d)\n",
			    DEVNAME(sc), err);
			return err;
		}
		sc->sc_flags &= ~IWX_FLAG_MAC_ACTIVE;
	}

	if (sc->sc_flags & IWX_FLAG_PHY_ACTIVE) {
		err = iwx_phy_ctxt_cmd(sc, &sc->sc_phyctxt[0], 1, 1,
		    IWX_FW_CTXT_ACTION_REMOVE, 0, 0, 0);
		if (err) {
			printf("%s: could not remove PHY context (error %d)\n",
			    DEVNAME(sc), err);
			return err;
		}
		sc->sc_flags &= ~IWX_FLAG_PHY_ACTIVE;
	}

	in->in_phyctxt = NULL;

	return 0;
}

static int
iwx_run(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	int err;

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		/* Add a MAC context and a sniffing STA. */
		err = iwx_auth(sc);
		if (err)
			return err;
	}

	/* Update STA again (association ID is now known). */
	err = iwx_add_sta_cmd(sc, in, 1);
	if (err) {
		printf("%s: could not update STA (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	/* We have now been assigned an associd by the AP. */
	err = iwx_mac_ctxt_cmd(sc, in, IWX_FW_CTXT_ACTION_MODIFY, 1);
	if (err) {
		printf("%s: failed to update MAC\n", DEVNAME(sc));
		return err;
	}

	err = iwx_sf_config(sc, IWX_SF_FULL_ON);
	if (err) {
		printf("%s: could not set sf full on (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	err = iwx_allow_mcast(sc);
	if (err) {
		printf("%s: could not allow mcast (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	/* LISPBSD: `ifconfig iwx0 powersave` selects PS level 2, else CAM. */
	err = iwx_set_pslevel(sc, 0,
	    (ic->ic_flags & IEEE80211_F_PMGTON) ? 2 : 0, 1);
	if (err) {
		printf("%s: could not send power command (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	if (ic->ic_opmode == IEEE80211_M_MONITOR)
		return 0;

	/* Start at lowest available bit-rate. Firmware will raise. */
	in->in_ni.ni_txrate = 0;

	err = iwx_rs_init(sc, in);
	if (err) {
		printf("%s: could not init rate scaling (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	return 0;
}

static int
iwx_run_stop(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	int err;

	err = iwx_flush_sta(sc, in);
	if (err) {
		printf("%s: could not flush Tx path (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	err = iwx_sf_config(sc, IWX_SF_INIT_OFF);
	if (err)
		return err;

	err = iwx_disable_beacon_filter(sc);
	if (err) {
		printf("%s: could not disable beacon filter (error %d)\n",
		    DEVNAME(sc), err);
		return err;
	}

	/* Mark station as disassociated. */
	err = iwx_mac_ctxt_cmd(sc, in, IWX_FW_CTXT_ACTION_MODIFY, 0);
	if (err) {
		printf("%s: failed to update MAC\n", DEVNAME(sc));
		return err;
	}

	return 0;
}

static struct ieee80211_node *
iwx_node_alloc(struct ieee80211_node_table *nt)
{
	return malloc(sizeof(struct iwx_node), M_80211_NODE, M_NOWAIT | M_ZERO);
}

static int
iwx_media_change(struct ifnet *ifp)
{
	int err;

	err = ieee80211_media_change(ifp);
	if (err != ENETRESET)
		return err;

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING)) {
		iwx_stop_locked(ifp);
		err = iwx_init_locked(ifp);
	}
	return err;
}

static void
iwx_newstate_task(void *psc)
{
	struct iwx_softc *sc = (struct iwx_softc *)psc;
	struct ieee80211com *ic = &sc->sc_ic;
	enum ieee80211_state nstate = sc->ns_nstate;
	enum ieee80211_state ostate = ic->ic_state;
	int arg = sc->ns_arg;
	int err = 0, s = splnet();

	if (sc->sc_flags & IWX_FLAG_SHUTDOWN) {
		/* iwx_stop() is waiting for us. */
		iwx_refcnt_rele_wake(sc);
		splx(s);
		return;
	}

	if (ostate == IEEE80211_S_SCAN) {
		if (nstate == ostate) {
			if (sc->sc_flags & IWX_FLAG_SCANNING) {
				iwx_refcnt_rele_wake(sc);
				splx(s);
				return;
			}
			/* Firmware is no longer scanning. Do another scan. */
			goto next_scan;
		}
	}

	if (nstate <= ostate) {
		switch (ostate) {
		case IEEE80211_S_RUN:
			/*
			 * net80211 sends a DISASSOC frame when it processes
			 * the RUN -> INIT transition, but by then we have
			 * already removed the station and its Tx queue from
			 * the firmware and iwx_tx() will drop the frame.
			 * Tell the AP that we are leaving while we still can;
			 * iwx_run_stop() flushes the Tx path afterwards.
			 */
			if (nstate == IEEE80211_S_INIT &&
			    ic->ic_opmode == IEEE80211_M_STA &&
			    ic->ic_bss != NULL) {
				IEEE80211_SEND_MGMT(ic, ic->ic_bss,
				    IEEE80211_FC0_SUBTYPE_DISASSOC,
				    IEEE80211_REASON_ASSOC_LEAVE);
			}
			err = iwx_run_stop(sc);
			if (err)
				goto out;
			/* FALLTHROUGH */
		case IEEE80211_S_ASSOC:
		case IEEE80211_S_AUTH:
			if (nstate <= IEEE80211_S_AUTH) {
				err = iwx_deauth(sc);
				if (err)
					goto out;
			}
			/* FALLTHROUGH */
		case IEEE80211_S_SCAN:
		case IEEE80211_S_INIT:
			break;
		}

		/* Die now if iwx_stop() was called while we were sleeping. */
		if (sc->sc_flags & IWX_FLAG_SHUTDOWN) {
			iwx_refcnt_rele_wake(sc);
			splx(s);
			return;
		}
	}

	switch (nstate) {
	case IEEE80211_S_INIT:
		break;

	case IEEE80211_S_SCAN:
next_scan:
		err = iwx_scan(sc, ostate);
		if (err)
			break;
		iwx_refcnt_rele_wake(sc);
		splx(s);
		return;

	case IEEE80211_S_AUTH:
		err = iwx_auth(sc);
		break;

	case IEEE80211_S_ASSOC:
		break;

	case IEEE80211_S_RUN:
		err = iwx_run(sc);
		break;
	}

out:
	if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0) {
		if (err)
			iwx_task_add(sc->sc_systq, &sc->init_task);
		else
			sc->sc_newstate(ic, nstate, arg);
	}
	iwx_refcnt_rele_wake(sc);
	splx(s);
}

static int
iwx_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct ifnet *ifp = IC2IFP(ic);
	struct iwx_softc *sc = ifp->if_softc;

	/*
	 * Prevent attempts to transition towards the same state, unless
	 * we are scanning in which case a SCAN -> SCAN transition
	 * triggers another scan iteration. And AUTH -> AUTH is needed
	 * to support band-steering.
	 */
	if (sc->ns_nstate == nstate && nstate != IEEE80211_S_SCAN &&
	    nstate != IEEE80211_S_AUTH)
		return 0;

	sc->ns_nstate = nstate;
	sc->ns_arg = arg;

	iwx_add_task(sc, sc->sc_nswq, &sc->newstate_task);

	return 0;
}

static void
iwx_endscan(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	if ((sc->sc_flags & (IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN)) == 0)
		return;

	sc->sc_flags &= ~(IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN);
	if (ic->ic_state == IEEE80211_S_SCAN)
		ieee80211_end_scan(ic);
}

/*
 * Aging and idle timeouts for the different possible scenarios
 * in default configuration
 */
static const uint32_t
iwx_sf_full_timeout_def[IWX_SF_NUM_SCENARIO][IWX_SF_NUM_TIMEOUT_TYPES] = {
	{
		htole32(IWX_SF_SINGLE_UNICAST_AGING_TIMER_DEF),
		htole32(IWX_SF_SINGLE_UNICAST_IDLE_TIMER_DEF)
	},
	{
		htole32(IWX_SF_AGG_UNICAST_AGING_TIMER_DEF),
		htole32(IWX_SF_AGG_UNICAST_IDLE_TIMER_DEF)
	},
	{
		htole32(IWX_SF_MCAST_AGING_TIMER_DEF),
		htole32(IWX_SF_MCAST_IDLE_TIMER_DEF)
	},
	{
		htole32(IWX_SF_BA_AGING_TIMER_DEF),
		htole32(IWX_SF_BA_IDLE_TIMER_DEF)
	},
	{
		htole32(IWX_SF_TX_RE_AGING_TIMER_DEF),
		htole32(IWX_SF_TX_RE_IDLE_TIMER_DEF)
	},
};

/*
 * Aging and idle timeouts for the different possible scenarios
 * in single BSS MAC configuration.
 */
static const uint32_t
iwx_sf_full_timeout[IWX_SF_NUM_SCENARIO][IWX_SF_NUM_TIMEOUT_TYPES] = {
	{
		htole32(IWX_SF_SINGLE_UNICAST_AGING_TIMER),
		htole32(IWX_SF_SINGLE_UNICAST_IDLE_TIMER)
	},
	{
		htole32(IWX_SF_AGG_UNICAST_AGING_TIMER),
		htole32(IWX_SF_AGG_UNICAST_IDLE_TIMER)
	},
	{
		htole32(IWX_SF_MCAST_AGING_TIMER),
		htole32(IWX_SF_MCAST_IDLE_TIMER)
	},
	{
		htole32(IWX_SF_BA_AGING_TIMER),
		htole32(IWX_SF_BA_IDLE_TIMER)
	},
	{
		htole32(IWX_SF_TX_RE_AGING_TIMER),
		htole32(IWX_SF_TX_RE_IDLE_TIMER)
	},
};

static void
iwx_fill_sf_command(struct iwx_softc *sc, struct iwx_sf_cfg_cmd *sf_cmd,
    struct ieee80211_node *ni)
{
	int i, j, watermark;

	sf_cmd->watermark[IWX_SF_LONG_DELAY_ON] = htole32(IWX_SF_W_MARK_SCAN);

	/*
	 * If we are in association flow - check antenna configuration
	 * capabilities of the AP station, and choose the watermark accordingly.
	 */
	if (ni) {
		watermark = IWX_SF_W_MARK_LEGACY;
	/* default watermark value for unassociated mode. */
	} else {
		watermark = IWX_SF_W_MARK_MIMO2;
	}
	sf_cmd->watermark[IWX_SF_FULL_ON] = htole32(watermark);

	for (i = 0; i < IWX_SF_NUM_SCENARIO; i++) {
		for (j = 0; j < IWX_SF_NUM_TIMEOUT_TYPES; j++) {
			sf_cmd->long_delay_timeouts[i][j] =
					htole32(IWX_SF_LONG_DELAY_AGING_TIMER);
		}
	}

	if (ni) {
		memcpy(sf_cmd->full_on_timeouts, iwx_sf_full_timeout,
		       sizeof(iwx_sf_full_timeout));
	} else {
		memcpy(sf_cmd->full_on_timeouts, iwx_sf_full_timeout_def,
		       sizeof(iwx_sf_full_timeout_def));
	}

}

static int
iwx_sf_config(struct iwx_softc *sc, int new_state)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_sf_cfg_cmd sf_cmd = {
		.state = htole32(new_state),
	};
	int err = 0;

	if (isset(sc->sc_ucode_api, IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD))
		return 0;

	switch (new_state) {
	case IWX_SF_UNINIT:
	case IWX_SF_INIT_OFF:
		iwx_fill_sf_command(sc, &sf_cmd, NULL);
		break;
	case IWX_SF_FULL_ON:
		iwx_fill_sf_command(sc, &sf_cmd, ic->ic_bss);
		break;
	default:
		return EINVAL;
	}

	err = iwx_send_cmd_pdu(sc, IWX_REPLY_SF_CFG_CMD, IWX_CMD_ASYNC,
				   sizeof(sf_cmd), &sf_cmd);
	return err;
}

static int
iwx_send_bt_init_conf(struct iwx_softc *sc)
{
	struct iwx_bt_coex_cmd bt_cmd;

	bt_cmd.mode = htole32(IWX_BT_COEX_WIFI);
	bt_cmd.enabled_modules = 0;

	return iwx_send_cmd_pdu(sc, IWX_BT_CONFIG, 0, sizeof(bt_cmd),
	    &bt_cmd);
}

static int
iwx_send_soc_conf(struct iwx_softc *sc)
{
	struct iwx_soc_configuration_cmd cmd;
	int err;
	uint32_t cmd_id, flags = 0;

	memset(&cmd, 0, sizeof(cmd));

	/*
	 * In VER_1 of this command, the discrete value is considered
	 * an integer; In VER_2, it's a bitmask.  Since we have only 2
	 * values in VER_1, this is backwards-compatible with VER_2,
	 * as long as we don't set any other flag bits.
	 */
	if (!sc->sc_integrated) { /* VER_1 */
		flags = IWX_SOC_CONFIG_CMD_FLAGS_DISCRETE;
	} else { /* VER_2 */
		uint8_t scan_cmd_ver;
		if (sc->sc_ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_NONE)
			flags |= (sc->sc_ltr_delay &
			    IWX_SOC_FLAGS_LTR_APPLY_DELAY_MASK);
		scan_cmd_ver = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP,
		    IWX_SCAN_REQ_UMAC);
		if (scan_cmd_ver != IWX_FW_CMD_VER_UNKNOWN &&
		    scan_cmd_ver >= 2 && sc->sc_low_latency_xtal)
			flags |= IWX_SOC_CONFIG_CMD_FLAGS_LOW_LATENCY;
	}
	cmd.flags = htole32(flags);

	cmd.latency = htole32(sc->sc_xtal_latency);

	cmd_id = iwx_cmd_id(IWX_SOC_CONFIGURATION_CMD, IWX_SYSTEM_GROUP, 0);
	err = iwx_send_cmd_pdu(sc, cmd_id, 0, sizeof(cmd), &cmd);
	if (err)
		printf("%s: failed to set soc latency: %d\n", DEVNAME(sc), err);
	return err;
}

static int
iwx_send_update_mcc_cmd(struct iwx_softc *sc, const char *alpha2)
{
	struct iwx_mcc_update_cmd mcc_cmd;
	struct iwx_host_cmd hcmd = {
		.id = IWX_MCC_UPDATE_CMD,
		.flags = IWX_CMD_WANT_RESP,
		.data = { &mcc_cmd },
	};
	struct iwx_rx_packet *pkt;
	size_t resp_len;
	int err, resp_version;

	resp_version = iwx_lookup_notif_ver(sc, IWX_LONG_GROUP,
	    IWX_MCC_UPDATE_CMD);
	if (resp_version >= 8) {
		printf("%s: unsupported MCC update command response "
		    "version %u\n", DEVNAME(sc), resp_version);
		return ENOTSUP;
	}

	memset(&mcc_cmd, 0, sizeof(mcc_cmd));
	mcc_cmd.mcc = htole16(alpha2[0] << 8 | alpha2[1]);
	if (isset(sc->sc_ucode_api, IWX_UCODE_TLV_API_WIFI_MCC_UPDATE) ||
	    isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC))
		mcc_cmd.source_id = IWX_MCC_SOURCE_GET_CURRENT;
	else
		mcc_cmd.source_id = IWX_MCC_SOURCE_OLD_FW;

	hcmd.len[0] = sizeof(struct iwx_mcc_update_cmd);
	hcmd.resp_pkt_len = IWX_CMD_RESP_MAX;

	err = iwx_send_cmd(sc, &hcmd);
	if (err)
		return err;

	pkt = hcmd.resp_pkt;
	if (!pkt || (pkt->hdr.flags & IWX_CMD_FAILED_MSK)) {
		err = EIO;
		goto out;
	}

	resp_len = iwx_rx_packet_payload_len(pkt);


	if (isset(sc->sc_enabled_capa,
	    IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT)) {
		struct iwx_mcc_update_resp_v4 *resp;

		if (resp_len < sizeof(*resp)) {
			err = EIO;
			goto out;
		}

		resp = (void *)pkt->data;
		if (resp_len != sizeof(*resp) +
		    resp->n_channels * sizeof(resp->channels[0])) {
			err = EIO;
			goto out;
		}

		DPRINTF(("MCC status=0x%x mcc=0x%x cap=0x%x time=0x%x "
		    "geo_info=0x%x source_id=0x%d n_channels=%u\n",
		    resp->status, resp->mcc, resp->cap, resp->time,
		    resp->geo_info, resp->source_id, resp->n_channels));

		/* Update channel map and our scan configuration. */
		iwx_init_channel_map(sc, NULL, resp->channels,
		    resp->n_channels);
	} else {
		struct iwx_mcc_update_resp *resp;

		if (resp_len < sizeof(*resp)) {
			err = EIO;
			goto out;
		}
		resp = (void *)pkt->data;
		if (resp_len != sizeof(*resp) +
		    resp->n_channels * sizeof(resp->channels[0])) {
			err = EIO;
			goto out;
		}

		DPRINTF(("MCC status=0x%x mcc=0x%x cap=0x%x time=0x%x "
		    "geo_info=0x%x source_id=0x%d n_channels=%u\n",
		    resp->status, resp->mcc, resp->cap, resp->time,
		    resp->geo_info, resp->source_id, resp->n_channels));

		/* Update channel map and our scan configuration. */
		iwx_init_channel_map(sc, NULL, resp->channels,
		    resp->n_channels);
	}

out:
	iwx_free_resp(sc, &hcmd);

	return err;
}

static int
iwx_send_temp_report_ths_cmd(struct iwx_softc *sc)
{
	struct iwx_temp_report_ths_cmd cmd;
	int err;

	/*
	 * In order to give responsibility for critical-temperature-kill
	 * and TX backoff to FW we need to send an empty temperature
	 * reporting command at init time.
	 */
	memset(&cmd, 0, sizeof(cmd));

	err = iwx_send_cmd_pdu(sc,
	    IWX_WIDE_ID(IWX_PHY_OPS_GROUP, IWX_TEMP_REPORTING_THRESHOLDS_CMD),
	    0, sizeof(cmd), &cmd);
	if (err)
		printf("%s: TEMP_REPORT_THS_CMD command failed (error %d)\n",
		    DEVNAME(sc), err);

	return err;
}

static int
iwx_init_hw(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	int err, i;

	err = iwx_run_init_mvm_ucode(sc, 0);
	if (err)
		return err;

	if (!iwx_nic_lock(sc))
		return EBUSY;

	err = iwx_send_tx_ant_cfg(sc, iwx_fw_valid_tx_ant(sc));
	if (err) {
		printf("%s: could not init tx ant config (error %d)\n",
		    DEVNAME(sc), err);
		goto err;
	}

	if (sc->sc_tx_with_siso_diversity) {
		err = iwx_send_phy_cfg_cmd(sc);
		if (err) {
			printf("%s: could not send phy config (error %d)\n",
			    DEVNAME(sc), err);
			goto err;
		}
	}

	err = iwx_send_bt_init_conf(sc);
	if (err) {
		printf("%s: could not init bt coex (error %d)\n",
		    DEVNAME(sc), err);
		goto err;
	}

	err = iwx_send_soc_conf(sc);
	if (err)
		goto err;

	if (isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_DQA_SUPPORT)) {
		err = iwx_send_dqa_cmd(sc);
		if (err)
			goto err;
	}

	for (i = 0; i < IWX_NUM_PHY_CTX; i++) {
		/*
		 * The channel used here isn't relevant as it's
		 * going to be overwritten in the other flows.
		 * For now use the first channel we have.
		 */
		sc->sc_phyctxt[i].id = i;
		sc->sc_phyctxt[i].channel = &ic->ic_channels[1];
	}

	err = iwx_config_ltr(sc);
	if (err) {
		printf("%s: PCIe LTR configuration failed (error %d)\n",
		    DEVNAME(sc), err);
	}

	if (isset(sc->sc_enabled_capa, IWX_UCODE_TLV_CAPA_CT_KILL_BY_FW)) {
		err = iwx_send_temp_report_ths_cmd(sc);
		if (err)
			goto err;
	}

	err = iwx_set_pslevel(sc, 0,
	    (ic->ic_flags & IEEE80211_F_PMGTON) ? 2 : 0, 0);
	if (err) {
		printf("%s: could not send power command (error %d)\n",
		    DEVNAME(sc), err);
		goto err;
	}

	if (sc->sc_nvm.lar_enabled) {
		err = iwx_send_update_mcc_cmd(sc, "ZZ");
		if (err) {
			printf("%s: could not init LAR (error %d)\n",
			    DEVNAME(sc), err);
			goto err;
		}
	}

	err = iwx_config_umac_scan_reduced(sc);
	if (err) {
		printf("%s: could not configure scan (error %d)\n",
		    DEVNAME(sc), err);
		goto err;
	}

	err = iwx_disable_beacon_filter(sc);
	if (err) {
		printf("%s: could not disable beacon filter (error %d)\n",
		    DEVNAME(sc), err);
		goto err;
	}

err:
	iwx_nic_unlock(sc);
	return err;
}

/* Allow multicast from our BSSID. */
static int
iwx_allow_mcast(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	struct iwx_mcast_filter_cmd *cmd;
	size_t size;
	int err;

	size = roundup(sizeof(*cmd), 4);
	cmd = kmem_intr_zalloc(size, KM_NOSLEEP);
	if (cmd == NULL)
		return ENOMEM;
	cmd->filter_own = 1;
	cmd->port_id = 0;
	cmd->count = 0;
	cmd->pass_all = 1;
	IEEE80211_ADDR_COPY(cmd->bssid, in->in_macaddr);

	err = iwx_send_cmd_pdu(sc, IWX_MCAST_FILTER_CMD,
	    0, size, cmd);
	kmem_intr_free(cmd, size);
	return err;
}

static int
iwx_init_locked(struct ifnet *ifp)
{
	struct iwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int err, generation;

	KASSERT(rw_write_held(&sc->ioctl_rwl));

	if (!ISSET(sc->sc_flags, IWX_FLAG_ATTACHED))
		return ENXIO;

	generation = ++sc->sc_generation;

	err = iwx_preinit(sc);
	if (err)
		return err;

	err = iwx_start_hw(sc);
	if (err) {
		printf("%s: could not initialize hardware\n", DEVNAME(sc));
		return err;
	}

	err = iwx_init_hw(sc);
	if (err) {
		if (generation == sc->sc_generation)
			iwx_stop_device(sc);
		return err;
	}

	KASSERT(sc->task_refs >= 0);
	iwx_refcnt_take(sc);	/* base reference released in iwx_stop() */
	ifp->if_flags &= ~IFF_OACTIVE;
	ifp->if_flags |= IFF_RUNNING;

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		ic->ic_bss->ni_chan = ic->ic_ibss_chan;
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		return 0;
	}

	ieee80211_begin_scan(ic, 0);

	/*
	 * ieee80211_begin_scan() ends up scheduling iwx_newstate_task().
	 * Wait until the transition to SCAN state has completed.
	 */
	do {
		err = tsleep(&ic->ic_state, PCATCH, "iwxinit",
		    SEC_TO_TICKS(1));
		if (generation != sc->sc_generation)
			return ENXIO;
		if (err) {
			iwx_stop_locked(ifp);
			return err;
		}
	} while (ic->ic_state != IEEE80211_S_SCAN);

	return 0;
}

static int
iwx_init(struct ifnet *ifp)
{
	struct iwx_softc *sc = ifp->if_softc;
	int err, held;

	held = rw_write_held(&sc->ioctl_rwl);
	if (!held)
		rw_enter(&sc->ioctl_rwl, RW_WRITER);
	err = iwx_init_locked(ifp);
	if (!held)
		rw_exit(&sc->ioctl_rwl);
	return err;
}

static void
iwx_start(struct ifnet *ifp)
{
	struct iwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ether_header *eh;
	struct mbuf *m;

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	for (;;) {
		/* why isn't this done per-queue? */
		if (sc->qfullmsk != 0) {
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		/* Don't queue additional frames while flushing Tx queues. */
		if (sc->sc_flags & IWX_FLAG_TXFLUSH)
			break;

		/* need to send management frames even if we're not RUNning */
		IF_DEQUEUE(&ic->ic_mgtq, m);
		if (m) {
			ni = M_GETCTX(m, struct ieee80211_node *);
			M_CLEARCTX(m);
			goto sendit;
		}

		if (ic->ic_state != IEEE80211_S_RUN)
			break;

		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;

		if (m->m_len < (int)sizeof (*eh) &&
		    (m = m_pullup(m, sizeof (*eh))) == NULL) {
			if_statinc(ifp, if_oerrors);
			continue;
		}

		eh = mtod(m, struct ether_header *);
		ni = ieee80211_find_txnode(ic, eh->ether_dhost);
		if (ni == NULL) {
			m_freem(m);
			if_statinc(ifp, if_oerrors);
			continue;
		}

		/* classify mbuf so we can find which tx ring to use */
		if (ieee80211_classify(ic, m, ni) != 0) {
			m_freem(m);
			ieee80211_free_node(ni);
			if_statinc(ifp, if_oerrors);
			continue;
		}

		bpf_mtap(ifp, m, BPF_D_OUT);

		if ((m = ieee80211_encap(ic, m, ni)) == NULL) {
			ieee80211_free_node(ni);
			if_statinc(ifp, if_oerrors);
			continue;
		}

 sendit:
		bpf_mtap3(ic->ic_rawbpf, m, BPF_D_OUT);

		if (iwx_tx(sc, m, ni) != 0) {
			ieee80211_free_node(ni);
			if_statinc(ifp, if_oerrors);
			continue;
		}

		if (ifp->if_flags & IFF_UP)
			ifp->if_timer = 1;
	}
}

static void
iwx_stop_locked(struct ifnet *ifp)
{
	struct iwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct iwx_node *in = (void *)ic->ic_bss;
	int i, s = splnet();

	KASSERT(rw_write_held(&sc->ioctl_rwl));

	sc->sc_flags |= IWX_FLAG_SHUTDOWN; /* Disallow new tasks. */

	/* Cancel scheduled tasks and let any stale tasks finish up. */
	iwx_task_del(sc->sc_systq, &sc->init_task);
	iwx_del_task(sc, sc->sc_nswq, &sc->newstate_task);
	if (ifp->if_flags & IFF_RUNNING) {
		/* Drop the base reference taken by iwx_init() and wait. */
		iwx_refcnt_finalize(sc, "iwxstop");
	} else {
		while (sc->task_refs > 0)
			tsleep(&sc->task_refs, 0, "iwxstop", 0);
	}

	iwx_stop_device(sc);

	/* Reset soft state. */

	sc->sc_generation++;
	for (i = 0; i < nitems(sc->sc_cmd_resp_pkt); i++) {
		if (sc->sc_cmd_resp_pkt[i] != NULL)
			kmem_intr_free(sc->sc_cmd_resp_pkt[i],
			    sc->sc_cmd_resp_len[i]);
		sc->sc_cmd_resp_pkt[i] = NULL;
		sc->sc_cmd_resp_len[i] = 0;
	}
	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);

	if (in != NULL) {
		in->in_phyctxt = NULL;
		in->in_flags = 0;
		IEEE80211_ADDR_COPY(in->in_macaddr, iwx_etheranyaddr);
	}

	sc->sc_flags &= ~(IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN);
	sc->sc_flags &= ~IWX_FLAG_MAC_ACTIVE;
	sc->sc_flags &= ~IWX_FLAG_BINDING_ACTIVE;
	sc->sc_flags &= ~IWX_FLAG_STA_ACTIVE;
	sc->sc_flags &= ~IWX_FLAG_TE_ACTIVE;
	sc->sc_flags &= ~IWX_FLAG_HW_ERR;
	sc->sc_flags &= ~IWX_FLAG_SHUTDOWN;
	sc->sc_flags &= ~IWX_FLAG_TXFLUSH;
	sc->sc_flags &= ~IWX_FLAG_PHY_ACTIVE;

	sc->sc_rx_ba_sessions = 0;
	memset(sc->aggqid, 0, sizeof(sc->aggqid));

	if (ic->ic_state != IEEE80211_S_INIT)
		sc->sc_newstate(ic, IEEE80211_S_INIT, -1);
	sc->ns_nstate = IEEE80211_S_INIT;

	memset(sc->sc_tx_timer, 0, sizeof(sc->sc_tx_timer));
	ifp->if_timer = 0;

	splx(s);
}

static void
iwx_stop(struct ifnet *ifp, int disable)
{
	struct iwx_softc *sc = ifp->if_softc;
	int held;

	held = rw_write_held(&sc->ioctl_rwl);
	if (!held)
		rw_enter(&sc->ioctl_rwl, RW_WRITER);
	iwx_stop_locked(ifp);
	if (!held)
		rw_exit(&sc->ioctl_rwl);
}

static void
iwx_watchdog(struct ifnet *ifp)
{
	struct iwx_softc *sc = ifp->if_softc;
	int i;

	ifp->if_timer = 0;

	/*
	 * We maintain a separate timer for each Tx queue because
	 * Tx aggregation queues can get "stuck" while other queues
	 * keep working. The Linux driver uses a similar workaround.
	 */
	for (i = 0; i < nitems(sc->sc_tx_timer); i++) {
		if (sc->sc_tx_timer[i] > 0) {
			if (--sc->sc_tx_timer[i] == 0) {
				printf("%s: device timeout\n", DEVNAME(sc));
				if (ifp->if_flags & IFF_DEBUG) {
					iwx_nic_error(sc);
					iwx_dump_driver_status(sc);
				}
				if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0)
					iwx_task_add(sc->sc_systq,
					    &sc->init_task);
				if_statinc(ifp, if_oerrors);
				return;
			}
			ifp->if_timer = 1;
		}
	}

	ieee80211_watchdog(&sc->sc_ic);
}

static int
iwx_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct iwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	const struct sockaddr *sa;
	int s, err = 0, generation = sc->sc_generation;
	int held;

	/*
	 * Prevent processes from entering this function while another
	 * process is tsleep'ing in it. Some net80211 ioctl paths re-enter
	 * the driver's ioctl routine, so allow recursion from the same LWP.
	 */
	held = rw_write_held(&sc->ioctl_rwl);
	if (!held) {
		rw_enter(&sc->ioctl_rwl, RW_WRITER);
		if (generation != sc->sc_generation) {
			rw_exit(&sc->ioctl_rwl);
			return ENXIO;
		}
	}
	s = splnet();

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		err = ifioctl_common(ifp, cmd, data);
		if (err)
			break;
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING)) {
				/* Force reload of firmware image from disk. */
				sc->sc_fw.fw_status = IWX_FW_STATUS_NONE;
				err = iwx_init_locked(ifp);
				if (err)
					ifp->if_flags &= ~IFF_UP;
			}
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				iwx_stop_locked(ifp);
		}
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		sa = ifreq_getaddr(SIOCADDMULTI, (struct ifreq *)data);
		err = (cmd == SIOCADDMULTI) ?
		    ether_addmulti(sa, &sc->sc_ec) :
		    ether_delmulti(sa, &sc->sc_ec);
		if (err == ENETRESET)
			err = 0;
		break;

	default:
		err = ieee80211_ioctl(ic, cmd, data);
		break;
	}

	if (err == ENETRESET) {
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING)) {
			iwx_stop_locked(ifp);
			err = iwx_init_locked(ifp);
		}
	}

	splx(s);
	if (!held)
		rw_exit(&sc->ioctl_rwl);

	return err;
}

/*
 * Note: This structure is read from the device with IO accesses,
 * and the reading already does the endian conversion. As it is
 * read with uint32_t-sized accesses, any members with a different size
 * need to be ordered correctly though!
 */
struct iwx_error_event_table {
	uint32_t valid;		/* (nonzero) valid, (0) log is empty */
	uint32_t error_id;		/* type of error */
	uint32_t trm_hw_status0;	/* TRM HW status */
	uint32_t trm_hw_status1;	/* TRM HW status */
	uint32_t blink2;		/* branch link */
	uint32_t ilink1;		/* interrupt link */
	uint32_t ilink2;		/* interrupt link */
	uint32_t data1;		/* error-specific data */
	uint32_t data2;		/* error-specific data */
	uint32_t data3;		/* error-specific data */
	uint32_t bcon_time;		/* beacon timer */
	uint32_t tsf_low;		/* network timestamp function timer */
	uint32_t tsf_hi;		/* network timestamp function timer */
	uint32_t gp1;		/* GP1 timer register */
	uint32_t gp2;		/* GP2 timer register */
	uint32_t fw_rev_type;	/* firmware revision type */
	uint32_t major;		/* uCode version major */
	uint32_t minor;		/* uCode version minor */
	uint32_t hw_ver;		/* HW Silicon version */
	uint32_t brd_ver;		/* HW board version */
	uint32_t log_pc;		/* log program counter */
	uint32_t frame_ptr;		/* frame pointer */
	uint32_t stack_ptr;		/* stack pointer */
	uint32_t hcmd;		/* last host command header */
	uint32_t isr0;		/* isr status register LMPM_NIC_ISR0:
				 * rxtx_flag */
	uint32_t isr1;		/* isr status register LMPM_NIC_ISR1:
				 * host_flag */
	uint32_t isr2;		/* isr status register LMPM_NIC_ISR2:
				 * enc_flag */
	uint32_t isr3;		/* isr status register LMPM_NIC_ISR3:
				 * time_flag */
	uint32_t isr4;		/* isr status register LMPM_NIC_ISR4:
				 * wico interrupt */
	uint32_t last_cmd_id;	/* last HCMD id handled by the firmware */
	uint32_t wait_event;		/* wait event() caller address */
	uint32_t l2p_control;	/* L2pControlField */
	uint32_t l2p_duration;	/* L2pDurationField */
	uint32_t l2p_mhvalid;	/* L2pMhValidBits */
	uint32_t l2p_addr_match;	/* L2pAddrMatchStat */
	uint32_t lmpm_pmg_sel;	/* indicate which clocks are turned on
				 * (LMPM_PMG_SEL) */
	uint32_t u_timestamp;	/* indicate when the date and time of the
				 * compilation */
	uint32_t flow_handler;	/* FH read/write pointers, RX credit */
} __packed /* LOG_ERROR_TABLE_API_S_VER_3 */;

/*
 * UMAC error struct - relevant starting from family 8000 chip.
 * Note: This structure is read from the device with IO accesses,
 * and the reading already does the endian conversion. As it is
 * read with u32-sized accesses, any members with a different size
 * need to be ordered correctly though!
 */
struct iwx_umac_error_event_table {
	uint32_t valid;		/* (nonzero) valid, (0) log is empty */
	uint32_t error_id;	/* type of error */
	uint32_t blink1;	/* branch link */
	uint32_t blink2;	/* branch link */
	uint32_t ilink1;	/* interrupt link */
	uint32_t ilink2;	/* interrupt link */
	uint32_t data1;		/* error-specific data */
	uint32_t data2;		/* error-specific data */
	uint32_t data3;		/* error-specific data */
	uint32_t umac_major;
	uint32_t umac_minor;
	uint32_t frame_pointer;	/* core register 27*/
	uint32_t stack_pointer;	/* core register 28 */
	uint32_t cmd_header;	/* latest host cmd sent to UMAC */
	uint32_t nic_isr_pref;	/* ISR status register */
} __packed;

#define ERROR_START_OFFSET  (1 * sizeof(uint32_t))
#define ERROR_ELEM_SIZE     (7 * sizeof(uint32_t))

#define IWX_FW_SYSASSERT_CPU_MASK 0xf0000000
static const struct {
	const char *name;
	uint8_t num;
} advanced_lookup[] = {
	{ "NMI_INTERRUPT_WDG", 0x34 },
	{ "SYSASSERT", 0x35 },
	{ "UCODE_VERSION_MISMATCH", 0x37 },
	{ "BAD_COMMAND", 0x38 },
	{ "BAD_COMMAND", 0x39 },
	{ "NMI_INTERRUPT_DATA_ACTION_PT", 0x3C },
	{ "FATAL_ERROR", 0x3D },
	{ "NMI_TRM_HW_ERR", 0x46 },
	{ "NMI_INTERRUPT_TRM", 0x4C },
	{ "NMI_INTERRUPT_BREAK_POINT", 0x54 },
	{ "NMI_INTERRUPT_WDG_RXF_FULL", 0x5C },
	{ "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL", 0x64 },
	{ "NMI_INTERRUPT_HOST", 0x66 },
	{ "NMI_INTERRUPT_LMAC_FATAL", 0x70 },
	{ "NMI_INTERRUPT_UMAC_FATAL", 0x71 },
	{ "NMI_INTERRUPT_OTHER_LMAC_FATAL", 0x73 },
	{ "NMI_INTERRUPT_ACTION_PT", 0x7C },
	{ "NMI_INTERRUPT_UNKNOWN", 0x84 },
	{ "NMI_INTERRUPT_INST_ACTION_PT", 0x86 },
	{ "ADVANCED_SYSASSERT", 0 },
};

static const char *
iwx_desc_lookup(uint32_t num)
{
	int i;

	for (i = 0; i < nitems(advanced_lookup) - 1; i++)
		if (advanced_lookup[i].num ==
		    (num & ~IWX_FW_SYSASSERT_CPU_MASK))
			return advanced_lookup[i].name;

	/* No entry matches 'num', so it is the last: ADVANCED_SYSASSERT */
	return advanced_lookup[i].name;
}

static void
iwx_nic_umac_error(struct iwx_softc *sc)
{
	struct iwx_umac_error_event_table table;
	uint32_t base;
	uint32_t min_base = 0x400000;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		min_base = 0xD0000;

	base = sc->sc_uc.uc_umac_error_event_table;
	if (base < min_base) {
		printf("%s: Invalid error log pointer 0x%08x\n",
		    DEVNAME(sc), base);
		return;
	}

	if (iwx_read_mem(sc, base, &table, sizeof(table)/sizeof(uint32_t))) {
		printf("%s: reading errlog failed\n", DEVNAME(sc));
		return;
	}

	if (ERROR_START_OFFSET <= table.valid * ERROR_ELEM_SIZE) {
		printf("%s: Start UMAC Error Log Dump:\n", DEVNAME(sc));
		printf("%s: Status: 0x%x, count: %d\n", DEVNAME(sc),
			sc->sc_flags, table.valid);
	}

	printf("%s: 0x%08X | %s\n", DEVNAME(sc), table.error_id,
		iwx_desc_lookup(table.error_id));
	printf("%s: 0x%08X | umac branchlink1\n", DEVNAME(sc), table.blink1);
	printf("%s: 0x%08X | umac branchlink2\n", DEVNAME(sc), table.blink2);
	printf("%s: 0x%08X | umac interruptlink1\n", DEVNAME(sc), table.ilink1);
	printf("%s: 0x%08X | umac interruptlink2\n", DEVNAME(sc), table.ilink2);
	printf("%s: 0x%08X | umac data1\n", DEVNAME(sc), table.data1);
	printf("%s: 0x%08X | umac data2\n", DEVNAME(sc), table.data2);
	printf("%s: 0x%08X | umac data3\n", DEVNAME(sc), table.data3);
	printf("%s: 0x%08X | umac major\n", DEVNAME(sc), table.umac_major);
	printf("%s: 0x%08X | umac minor\n", DEVNAME(sc), table.umac_minor);
	printf("%s: 0x%08X | frame pointer\n", DEVNAME(sc),
	    table.frame_pointer);
	printf("%s: 0x%08X | stack pointer\n", DEVNAME(sc),
	    table.stack_pointer);
	printf("%s: 0x%08X | last host cmd\n", DEVNAME(sc), table.cmd_header);
	printf("%s: 0x%08X | isr status reg\n", DEVNAME(sc),
	    table.nic_isr_pref);
}

/*
 * Support for dumping the error log seemed like a good idea ...
 * but it's mostly hex junk and the only sensible thing is the
 * hw/ucode revision (which we know anyway).  Since it's here,
 * I'll just leave it in, just in case e.g. the Intel guys want to
 * help us decipher some "ADVANCED_SYSASSERT" later.
 */
static void
iwx_nic_error(struct iwx_softc *sc)
{
	struct iwx_error_event_table table;
	uint32_t base;
	uint32_t min_base = 0x400000;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		min_base = 0xD0000;

	printf("%s: dumping device error log\n", DEVNAME(sc));
	base = sc->sc_uc.uc_lmac_error_event_table[0];
	if (base < min_base) {
		printf("%s: Invalid error log pointer 0x%08x\n",
		    DEVNAME(sc), base);
		return;
	}

	if (iwx_read_mem(sc, base, &table, sizeof(table)/sizeof(uint32_t))) {
		printf("%s: reading errlog failed\n", DEVNAME(sc));
		return;
	}

	if (!table.valid) {
		printf("%s: errlog not found, skipping\n", DEVNAME(sc));
		return;
	}

	if (ERROR_START_OFFSET <= table.valid * ERROR_ELEM_SIZE) {
		printf("%s: Start Error Log Dump:\n", DEVNAME(sc));
		printf("%s: Status: 0x%x, count: %d\n", DEVNAME(sc),
		    sc->sc_flags, table.valid);
	}

	printf("%s: 0x%08X | %-28s\n", DEVNAME(sc), table.error_id,
	    iwx_desc_lookup(table.error_id));
	printf("%s: %08X | trm_hw_status0\n", DEVNAME(sc),
	    table.trm_hw_status0);
	printf("%s: %08X | trm_hw_status1\n", DEVNAME(sc),
	    table.trm_hw_status1);
	printf("%s: %08X | branchlink2\n", DEVNAME(sc), table.blink2);
	printf("%s: %08X | interruptlink1\n", DEVNAME(sc), table.ilink1);
	printf("%s: %08X | interruptlink2\n", DEVNAME(sc), table.ilink2);
	printf("%s: %08X | data1\n", DEVNAME(sc), table.data1);
	printf("%s: %08X | data2\n", DEVNAME(sc), table.data2);
	printf("%s: %08X | data3\n", DEVNAME(sc), table.data3);
	printf("%s: %08X | beacon time\n", DEVNAME(sc), table.bcon_time);
	printf("%s: %08X | tsf low\n", DEVNAME(sc), table.tsf_low);
	printf("%s: %08X | tsf hi\n", DEVNAME(sc), table.tsf_hi);
	printf("%s: %08X | time gp1\n", DEVNAME(sc), table.gp1);
	printf("%s: %08X | time gp2\n", DEVNAME(sc), table.gp2);
	printf("%s: %08X | uCode revision type\n", DEVNAME(sc),
	    table.fw_rev_type);
	printf("%s: %08X | uCode version major\n", DEVNAME(sc),
	    table.major);
	printf("%s: %08X | uCode version minor\n", DEVNAME(sc),
	    table.minor);
	printf("%s: %08X | hw version\n", DEVNAME(sc), table.hw_ver);
	printf("%s: %08X | board version\n", DEVNAME(sc), table.brd_ver);
	printf("%s: %08X | hcmd\n", DEVNAME(sc), table.hcmd);
	printf("%s: %08X | isr0\n", DEVNAME(sc), table.isr0);
	printf("%s: %08X | isr1\n", DEVNAME(sc), table.isr1);
	printf("%s: %08X | isr2\n", DEVNAME(sc), table.isr2);
	printf("%s: %08X | isr3\n", DEVNAME(sc), table.isr3);
	printf("%s: %08X | isr4\n", DEVNAME(sc), table.isr4);
	printf("%s: %08X | last cmd Id\n", DEVNAME(sc), table.last_cmd_id);
	printf("%s: %08X | wait_event\n", DEVNAME(sc), table.wait_event);
	printf("%s: %08X | l2p_control\n", DEVNAME(sc), table.l2p_control);
	printf("%s: %08X | l2p_duration\n", DEVNAME(sc), table.l2p_duration);
	printf("%s: %08X | l2p_mhvalid\n", DEVNAME(sc), table.l2p_mhvalid);
	printf("%s: %08X | l2p_addr_match\n", DEVNAME(sc), table.l2p_addr_match);
	printf("%s: %08X | lmpm_pmg_sel\n", DEVNAME(sc), table.lmpm_pmg_sel);
	printf("%s: %08X | timestamp\n", DEVNAME(sc), table.u_timestamp);
	printf("%s: %08X | flow_handler\n", DEVNAME(sc), table.flow_handler);

	if (sc->sc_uc.uc_umac_error_event_table)
		iwx_nic_umac_error(sc);
}

static void
iwx_dump_driver_status(struct iwx_softc *sc)
{
	int i;

	printf("driver status:\n");
	for (i = 0; i < nitems(sc->txq); i++) {
		struct iwx_tx_ring *ring = &sc->txq[i];
		printf("  tx ring %2d: qid=%-2d cur=%-3d "
		    "cur_hw=%-3d queued=%-3d\n",
		    i, ring->qid, ring->cur, ring->cur_hw,
		    ring->queued);
	}
	printf("  rx ring: cur=%d\n", sc->rxq.cur);
	printf("  802.11 state %s\n",
	    ieee80211_state_name[sc->sc_ic.ic_state]);
}

#define SYNC_RESP_STRUCT(_var_, _pkt_)					\
do {									\
	bus_dmamap_sync(sc->sc_dmat, data->map, sizeof(*(_pkt_)),	\
	    sizeof(*(_var_)), BUS_DMASYNC_POSTREAD);			\
	_var_ = (void *)((_pkt_)+1);					\
} while (/*CONSTCOND*/0)

static int
iwx_rx_pkt_valid(struct iwx_rx_packet *pkt)
{
	int qid, idx, code;

	qid = pkt->hdr.qid & ~0x80;
	idx = pkt->hdr.idx;
	code = IWX_WIDE_ID(pkt->hdr.flags, pkt->hdr.code);

	return (!(qid == 0 && idx == 0 && code == 0) &&
	    pkt->len_n_flags != htole32(IWX_FH_RSCSR_FRAME_INVALID));
}

static void
iwx_rx_pkt(struct iwx_softc *sc, struct iwx_rx_data *data)
{
	struct ifnet *ifp = IC2IFP(&sc->sc_ic);
	struct iwx_rx_packet *pkt, *nextpkt;
	uint32_t offset = 0, nextoff = 0, nmpdu = 0, len;
	struct mbuf *m0, *m;
	const size_t minsz = sizeof(pkt->len_n_flags) + sizeof(pkt->hdr);
	int qid, idx, code, handled = 1;

	bus_dmamap_sync(sc->sc_dmat, data->map, 0, IWX_RBUF_SIZE,
	    BUS_DMASYNC_POSTREAD);

	m0 = data->m;
	while (m0 && offset + minsz < IWX_RBUF_SIZE) {
		pkt = (struct iwx_rx_packet *)(m0->m_data + offset);
		qid = pkt->hdr.qid;
		idx = pkt->hdr.idx;

		code = IWX_WIDE_ID(pkt->hdr.flags, pkt->hdr.code);

		if (!iwx_rx_pkt_valid(pkt))
			break;

		/*
		 * XXX Intel inside (tm)
		 * Any commands in the LONG_GROUP could actually be in the
		 * LEGACY group. Firmware API versions >= 50 reject commands
		 * in group 0, forcing us to use this hack.
		 */
		if (iwx_cmd_groupid(code) == IWX_LONG_GROUP &&
		    (qid & ~0x80) < nitems(sc->txq)) {
			struct iwx_tx_ring *ring = &sc->txq[qid & ~0x80];
			struct iwx_tx_data *txdata = &ring->data[idx];
			if (txdata->flags & IWX_TXDATA_FLAG_CMD_IS_NARROW)
				code = iwx_cmd_opcode(code);
		}

		len = sizeof(pkt->len_n_flags) + iwx_rx_packet_len(pkt);
		if (len < minsz || len > (IWX_RBUF_SIZE - offset))
			break;

		if (code == IWX_REPLY_RX_MPDU_CMD && ++nmpdu == 1) {
			/* Take mbuf m0 off the RX ring. */
			if (iwx_rx_addbuf(sc, IWX_RBUF_SIZE, sc->rxq.cur)) {
				if_statinc(ifp, if_ierrors);
				break;
			}
			KASSERT(data->m != m0);
		}

		switch (code) {
		case IWX_REPLY_RX_PHY_CMD:
			iwx_rx_rx_phy_cmd(sc, pkt, data);
			break;

		case IWX_REPLY_RX_MPDU_CMD: {
			size_t maxlen = IWX_RBUF_SIZE - offset - minsz;
			nextoff = offset +
			    roundup(len, IWX_FH_RSCSR_FRAME_ALIGN);
			nextpkt = (struct iwx_rx_packet *)
			    (m0->m_data + nextoff);
			/* AX210 devices ship only one packet per Rx buffer. */
			if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210 ||
			    nextoff + minsz >= IWX_RBUF_SIZE ||
			    !iwx_rx_pkt_valid(nextpkt)) {
				/* No need to copy last frame in buffer. */
				if (offset > 0)
					m_adj(m0, offset);
				iwx_rx_mpdu_mq(sc, m0, pkt->data, maxlen);
				m0 = NULL; /* stack owns m0 now; abort loop */
			} else {
				/*
				 * Create an mbuf which points to the current
				 * packet. Always copy from offset zero to
				 * preserve m_pkthdr.
				 */
				m = m_copym(m0, 0, M_COPYALL, M_DONTWAIT);
				if (m == NULL) {
					if_statinc(ifp, if_ierrors);
					m_freem(m0);
					m0 = NULL;
					break;
				}
				m_adj(m, offset);
				iwx_rx_mpdu_mq(sc, m, pkt->data, maxlen);
			}
 			break;
		}

		case IWX_BAR_FRAME_RELEASE:
			/* Block Ack is not supported by this port. */
			break;

		case IWX_TX_CMD:
			iwx_rx_tx_cmd(sc, pkt, data);
			break;

		case IWX_BA_NOTIF:
			/* Tx aggregation is not supported by this port. */
			break;

		case IWX_MISSED_BEACONS_NOTIFICATION:
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MISSED_BEACONS_NOTIF):
			iwx_rx_bmiss(sc, pkt, data);
			break;

		case IWX_MFUART_LOAD_NOTIFICATION:
			break;

		case IWX_ALIVE: {
			struct iwx_alive_resp_v4 *resp4;
			struct iwx_alive_resp_v5 *resp5;
			struct iwx_alive_resp_v6 *resp6;
			int notif_ver;

			notif_ver = iwx_lookup_notif_ver(sc,
			    IWX_LEGACY_GROUP, IWX_ALIVE);
			DPRINTF(("%s: firmware alive version %d\n", __func__, notif_ver));
			sc->sc_uc.uc_ok = 0;

			/*
			 * For v5 and above, we can check the version, for older
			 * versions we need to check the size.
			 */
			if (notif_ver == 6 || notif_ver == 7) {
				SYNC_RESP_STRUCT(resp6, pkt);
				if (iwx_rx_packet_payload_len(pkt) !=
				    sizeof(*resp6)) {
					sc->sc_uc.uc_intr = 1;
					wakeup(&sc->sc_uc);
					break;
				}
				sc->sc_uc.uc_lmac_error_event_table[0] = le32toh(
				    resp6->lmac_data[0].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_lmac_error_event_table[1] = le32toh(
				    resp6->lmac_data[1].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_log_event_table = le32toh(
				    resp6->lmac_data[0].dbg_ptrs.log_event_table_ptr);
				sc->sc_uc.uc_umac_error_event_table = le32toh(
				    resp6->umac_data.dbg_ptrs.error_info_addr);
				sc->sc_sku_id[0] =
				    le32toh(resp6->sku_id.data[0]);
				sc->sc_sku_id[1] =
				    le32toh(resp6->sku_id.data[1]);
				sc->sc_sku_id[2] =
				    le32toh(resp6->sku_id.data[2]);
				if (resp6->status == IWX_ALIVE_STATUS_OK)
					sc->sc_uc.uc_ok = 1;
			 } else if (notif_ver == 5) {
				SYNC_RESP_STRUCT(resp5, pkt);
				if (iwx_rx_packet_payload_len(pkt) !=
				    sizeof(*resp5)) {
					sc->sc_uc.uc_intr = 1;
					wakeup(&sc->sc_uc);
					break;
				}
				sc->sc_uc.uc_lmac_error_event_table[0] = le32toh(
				    resp5->lmac_data[0].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_lmac_error_event_table[1] = le32toh(
				    resp5->lmac_data[1].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_log_event_table = le32toh(
				    resp5->lmac_data[0].dbg_ptrs.log_event_table_ptr);
				sc->sc_uc.uc_umac_error_event_table = le32toh(
				    resp5->umac_data.dbg_ptrs.error_info_addr);
				sc->sc_sku_id[0] =
				    le32toh(resp5->sku_id.data[0]);
				sc->sc_sku_id[1] =
				    le32toh(resp5->sku_id.data[1]);
				sc->sc_sku_id[2] =
				    le32toh(resp5->sku_id.data[2]);
				if (resp5->status == IWX_ALIVE_STATUS_OK)
					sc->sc_uc.uc_ok = 1;
			} else if (iwx_rx_packet_payload_len(pkt) == sizeof(*resp4)) {
				SYNC_RESP_STRUCT(resp4, pkt);
				sc->sc_uc.uc_lmac_error_event_table[0] = le32toh(
				    resp4->lmac_data[0].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_lmac_error_event_table[1] = le32toh(
				    resp4->lmac_data[1].dbg_ptrs.error_event_table_ptr);
				sc->sc_uc.uc_log_event_table = le32toh(
				    resp4->lmac_data[0].dbg_ptrs.log_event_table_ptr);
				sc->sc_uc.uc_umac_error_event_table = le32toh(
				    resp4->umac_data.dbg_ptrs.error_info_addr);
				if (resp4->status == IWX_ALIVE_STATUS_OK)
					sc->sc_uc.uc_ok = 1;
			}

			sc->sc_uc.uc_intr = 1;
			wakeup(&sc->sc_uc);
			break;
		}

		case IWX_STATISTICS_NOTIFICATION: {
			struct iwx_notif_statistics *stats;
			size_t slen;
			SYNC_RESP_STRUCT(stats, pkt);
			/*
			 * LISPBSD: the firmware statistics notification may be
			 * shorter than our struct (it is fw-version dependent) and
			 * the packet can end near the 4K RX buffer boundary; copying
			 * sizeof(sc_stats) blindly ran off the DMA buffer into an
			 * unmapped page (panic: trap in iwx_notif_intr+0x490).
			 */
			slen = iwx_rx_packet_payload_len(pkt);
			if (slen > sizeof(sc->sc_stats))
				slen = sizeof(sc->sc_stats);
			memcpy(&sc->sc_stats, stats, slen);
			sc->sc_noise = iwx_get_noise(&sc->sc_stats.rx.general);
			break;
		}

		case IWX_DTS_MEASUREMENT_NOTIFICATION:
		case IWX_WIDE_ID(IWX_PHY_OPS_GROUP,
				 IWX_DTS_MEASUREMENT_NOTIF_WIDE):
		case IWX_WIDE_ID(IWX_PHY_OPS_GROUP,
				 IWX_TEMP_REPORTING_THRESHOLDS_CMD):
			break;

		case IWX_WIDE_ID(IWX_PHY_OPS_GROUP,
		    IWX_CT_KILL_NOTIFICATION): {
			struct iwx_ct_kill_notif *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			printf("%s: device at critical temperature (%u degC), "
			    "stopping device\n",
			    DEVNAME(sc), le16toh(notif->temperature));
			sc->sc_flags |= IWX_FLAG_HW_ERR;
			iwx_task_add(sc->sc_systq, &sc->init_task);
			break;
		}

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_SCD_QUEUE_CONFIG_CMD):
		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_RX_BAID_ALLOCATION_CONFIG_CMD):
		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_SEC_KEY_CMD):
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_SESSION_PROTECTION_CMD):
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_MAC_CONFIG_CMD):
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_LINK_CONFIG_CMD):
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_STA_CONFIG_CMD):
		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_STA_REMOVE_CMD):
		case IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,
		    IWX_NVM_GET_INFO):
		case IWX_ADD_STA_KEY:
		case IWX_MGMT_MCAST_KEY:
		case IWX_PHY_CONFIGURATION_CMD:
		case IWX_TX_ANT_CONFIGURATION_CMD:
		case IWX_ADD_STA:
		case IWX_MAC_CONTEXT_CMD:
		case IWX_REPLY_SF_CFG_CMD:
		case IWX_POWER_TABLE_CMD:
		case IWX_LTR_CONFIG:
		case IWX_PHY_CONTEXT_CMD:
		case IWX_BINDING_CONTEXT_CMD:
		case IWX_WIDE_ID(IWX_LONG_GROUP, IWX_SCAN_CFG_CMD):
		case IWX_WIDE_ID(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC):
		case IWX_WIDE_ID(IWX_LONG_GROUP, IWX_SCAN_ABORT_UMAC):
		case IWX_REPLY_BEACON_FILTERING_CMD:
		case IWX_MAC_PM_POWER_TABLE:
		case IWX_TIME_QUOTA_CMD:
		case IWX_REMOVE_STA:
		case IWX_TXPATH_FLUSH:
		case IWX_BT_CONFIG:
		case IWX_MCC_UPDATE_CMD:
		case IWX_TIME_EVENT_CMD:
		case IWX_STATISTICS_CMD:
		case IWX_SCD_QUEUE_CFG: {
			size_t pkt_len;

			if (idx >= nitems(sc->sc_cmd_resp_pkt) ||
			    sc->sc_cmd_resp_pkt[idx] == NULL)
				break;

			bus_dmamap_sync(sc->sc_dmat, data->map, 0,
			    sizeof(*pkt), BUS_DMASYNC_POSTREAD);

			pkt_len = sizeof(pkt->len_n_flags) +
			    iwx_rx_packet_len(pkt);

			if ((pkt->hdr.flags & IWX_CMD_FAILED_MSK) ||
			    pkt_len < sizeof(*pkt) ||
			    pkt_len > sc->sc_cmd_resp_len[idx]) {
				kmem_intr_free(sc->sc_cmd_resp_pkt[idx],
				    sc->sc_cmd_resp_len[idx]);
				sc->sc_cmd_resp_pkt[idx] = NULL;
				break;
			}

			bus_dmamap_sync(sc->sc_dmat, data->map, sizeof(*pkt),
			    pkt_len - sizeof(*pkt), BUS_DMASYNC_POSTREAD);
			memcpy(sc->sc_cmd_resp_pkt[idx], pkt, pkt_len);
			break;
		}

		case IWX_INIT_COMPLETE_NOTIF:
			sc->sc_init_complete |= IWX_INIT_COMPLETE;
			wakeup(&sc->sc_init_complete);
			break;

		case IWX_SCAN_COMPLETE_UMAC: {
			struct iwx_umac_scan_complete *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			(void)notif;
			iwx_endscan(sc);
			break;
		}

		case IWX_SCAN_ITERATION_COMPLETE_UMAC: {
			struct iwx_umac_scan_iter_complete_notif *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			(void)notif;
			iwx_endscan(sc);
			break;
		}

		case IWX_MCC_CHUB_UPDATE_CMD: {
			struct iwx_mcc_chub_notif *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			iwx_mcc_update(sc, notif);
			break;
		}

		case IWX_REPLY_ERROR: {
			struct iwx_error_resp *resp;
			SYNC_RESP_STRUCT(resp, pkt);
			printf("%s: firmware error 0x%x, cmd 0x%x\n",
				DEVNAME(sc), le32toh(resp->error_type),
				resp->cmd_id);
			break;
		}

		case IWX_TIME_EVENT_NOTIFICATION: {
			struct iwx_time_event_notif *notif;
			uint32_t action;
			SYNC_RESP_STRUCT(notif, pkt);

			if (sc->sc_time_event_uid != le32toh(notif->unique_id))
				break;
			action = le32toh(notif->action);
			if (action & IWX_TE_V2_NOTIF_HOST_EVENT_END)
				sc->sc_flags &= ~IWX_FLAG_TE_ACTIVE;
			break;
		}

		case IWX_PSM_UAPSD_AP_MISBEHAVING_NOTIFICATION:
			/* U-APSD is not used by this port. */
			break;

		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_SESSION_PROTECTION_NOTIF): {
			struct iwx_session_prot_notif *notif;
			uint32_t status, start, conf_id;

			SYNC_RESP_STRUCT(notif, pkt);

			status = le32toh(notif->status);
			start = le32toh(notif->start);
			conf_id = le32toh(notif->conf_id);
			/* Check for end of successful PROTECT_CONF_ASSOC. */
			if (status == 1 && start == 0 &&
			    conf_id == IWX_SESSION_PROTECT_CONF_ASSOC)
				sc->sc_flags &= ~IWX_FLAG_TE_ACTIVE;
			break;
		}

		case IWX_WIDE_ID(IWX_MAC_CONF_GROUP,
		    IWX_CHANNEL_SWITCH_START_NOTIF): {
			if (sc->sc_ic.ic_opmode != IEEE80211_M_STA ||
			    sc->sc_ic.ic_state != IEEE80211_S_RUN)
				break;

			if (ifp->if_flags & IFF_DEBUG)
				printf("%s: firmware channel switch "
				    "notification 0x%x\n",
				    DEVNAME(sc), code);

			if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0)
				iwx_task_add(sc->sc_systq, &sc->init_task);
			break;
		}

		case IWX_WIDE_ID(IWX_SYSTEM_GROUP,
		    IWX_FSEQ_VER_MISMATCH_NOTIFICATION):
		    break;

		/*
		 * Firmware versions 21 and 22 generate some DEBUG_LOG_MSG
		 * messages. Just ignore them for now.
		 */
		case IWX_DEBUG_LOG_MSG:
			break;

		case IWX_MCAST_FILTER_CMD:
			break;

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_DQA_ENABLE_CMD):
			break;

		case IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_SOC_CONFIGURATION_CMD):
			break;

		case IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_INIT_EXTENDED_CFG_CMD):
			break;

		case IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,
		    IWX_NVM_ACCESS_COMPLETE):
			break;

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_NO_DATA_NOTIF):
			break; /* happens in monitor mode; ignore for now */

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD):
			break;

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_TLC_MNG_UPDATE_NOTIF): {
			struct iwx_tlc_update_notif *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			if (iwx_rx_packet_payload_len(pkt) == sizeof(*notif))
				iwx_rs_update(sc, notif);
			break;
		}

		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RLC_CONFIG_CMD):
			break;

		/*
		 * Ignore for now. The Linux driver only acts on this request
		 * with 160Mhz channels in 11ax mode.
		 */
		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP,
		    IWX_THERMAL_DUAL_CHAIN_REQUEST):
			DPRINTF(("%s: thermal dual-chain request received\n",
			    DEVNAME(sc)));
			break;

		/* undocumented notification from iwx-ty-a0-gf-a0-77 image */
		case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, 0xf8):
			break;

		/* undocumented notification from iwx-bz-b0-gf-a0-92 image */
		case IWX_WIDE_ID(IWX_SYSTEM_GROUP, 0xfc):
			break;

		case IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP,
		    IWX_PNVM_INIT_COMPLETE):
			sc->sc_init_complete |= IWX_PNVM_COMPLETE;
			wakeup(&sc->sc_init_complete);
			break;

		case IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_SYSTEM_STATISTICS_CMD):
			break;
	
		case IWX_WIDE_ID(IWX_SYSTEM_GROUP,
		    IWX_SYSTEM_STATISTICS_END_NOTIF): {
			struct iwx_system_statistics_end_notif *notif;
			SYNC_RESP_STRUCT(notif, pkt);
			(void)notif;
			sc->sc_system_stats_cleared = 1;
			wakeup(&sc->sc_system_stats_cleared);
			break;
		}
	
		case IWX_WIDE_ID(IWX_STATISTICS_GROUP,
		    IWX_STATISTICS_OPER_NOTIF):
		case IWX_WIDE_ID(IWX_STATISTICS_GROUP,
		    IWX_STATISTICS_OPER_PART1_NOTIF):
			break;

		case IWX_WIDE_ID(IWX_BT_COEX_GROUP, IWX_PROFILE_NOTIF):
			break;

		default:
			handled = 0;
			printf("%s: unhandled firmware response 0x%x/0x%x "
			    "rx ring %d[%d]\n",
			    DEVNAME(sc), code, pkt->len_n_flags,
			    (qid & ~0x80), idx);
			break;
		}

		/*
		 * uCode sets bit 0x80 when it originates the notification,
		 * i.e. when the notification is not a direct response to a
		 * command sent by the driver.
		 * For example, uCode issues IWX_REPLY_RX when it sends a
		 * received frame to the driver.
		 */
		if (handled && !(qid & (1 << 7))) {
			iwx_cmd_done(sc, qid, idx, code);
		}

		offset += roundup(len, IWX_FH_RSCSR_FRAME_ALIGN);

		/* AX210 devices ship only one packet per Rx buffer. */
		if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
			break;
	}

	if (m0 && m0 != data->m)
		m_freem(m0);
}

static void
iwx_notif_intr(struct iwx_softc *sc)
{
	uint16_t hw;

	bus_dmamap_sync(sc->sc_dmat, sc->rxq.stat_dma.map,
	    0, sc->rxq.stat_dma.size, BUS_DMASYNC_POSTREAD);

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		uint16_t *status = sc->rxq.stat_dma.vaddr;
		hw = le16toh(*status) & 0xfff;
	} else
		hw = le16toh(sc->rxq.stat->closed_rb_num) & 0xfff;
	hw &= (IWX_RX_MQ_RING_COUNT - 1);
	while (sc->rxq.cur != hw) {
		struct iwx_rx_data *data = &sc->rxq.data[sc->rxq.cur];
		iwx_rx_pkt(sc, data);
		sc->rxq.cur = (sc->rxq.cur + 1) % IWX_RX_MQ_RING_COUNT;
	}

	/*
	 * Tell the firmware what we have processed.
	 * Seems like the hardware gets upset unless we align the write by 8??
	 */
	hw = (hw == 0) ? IWX_RX_MQ_RING_COUNT - 1 : hw - 1;
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
		IWX_WRITE(sc, IWX_HBUS_TARG_WRPTR,
		    (hw & ~7) | IWX_HBUS_TARG_WRPTR_RX_Q(0));
	} else
		IWX_WRITE(sc, IWX_RFH_Q0_FRBDCB_WIDX_TRG, hw & ~7);
}

/*
 * Interrupt handling. The hardware interrupt handlers only schedule the
 * soft interrupt; all work is done from softint context (like iwm(4)).
 */
static int
iwx_intr(void *arg)
{
	struct iwx_softc *sc = arg;

	/* Disable interrupts */
	IWX_WRITE(sc, IWX_CSR_INT_MASK, 0);

	softint_schedule(sc->sc_soft_ih);
	return 1;
}

static int
iwx_intr_msix(void *arg)
{
	struct iwx_softc *sc = arg;

	/* The hardware auto-masks the vector until we clear it. */
	softint_schedule(sc->sc_soft_ih);
	return 1;
}

static void
iwx_softintr_legacy(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	uint32_t r1, r2;

	if (sc->sc_flags & IWX_FLAG_USE_ICT) {
		uint32_t *ict = sc->ict_dma.vaddr;
		uint32_t tmp;

		bus_dmamap_sync(sc->sc_dmat, sc->ict_dma.map,
		    0, sc->ict_dma.size, BUS_DMASYNC_POSTREAD);
		tmp = htole32(ict[sc->ict_cur]);
		if (!tmp)
			goto out_ena;

		/*
		 * ok, there was something.  keep plowing until we have all.
		 */
		r1 = r2 = 0;
		while (tmp) {
			r1 |= tmp;
			ict[sc->ict_cur] = 0;
			sc->ict_cur = (sc->ict_cur+1) % IWX_ICT_COUNT;
			tmp = htole32(ict[sc->ict_cur]);
		}

		bus_dmamap_sync(sc->sc_dmat, sc->ict_dma.map,
		    0, sc->ict_dma.size, BUS_DMASYNC_PREWRITE);

		/* this is where the fun begins.  don't ask */
		if (r1 == 0xffffffff)
			r1 = 0;

		/* i am not expected to understand this */
		if (r1 & 0xc0000)
			r1 |= 0x8000;
		r1 = (0xff & r1) | ((0xff00 & r1) << 16);
	} else {
		r1 = IWX_READ(sc, IWX_CSR_INT);
		if (r1 == 0xffffffff || (r1 & 0xfffffff0) == 0xa5a5a5a0)
			return;
		r2 = IWX_READ(sc, IWX_CSR_FH_INT_STATUS);
	}
	if (r1 == 0 && r2 == 0) {
		goto out_ena;
	}

	IWX_WRITE(sc, IWX_CSR_INT, r1 | ~sc->sc_intmask);

	if (r1 & IWX_CSR_INT_BIT_ALIVE) {
		int i;

		/* Firmware has now configured the RFH. */
		for (i = 0; i < IWX_RX_MQ_RING_COUNT; i++)
			iwx_update_rx_desc(sc, &sc->rxq, i);
		IWX_WRITE(sc, IWX_RFH_Q0_FRBDCB_WIDX_TRG, 8);
	}

	if (r1 & IWX_CSR_INT_BIT_RF_KILL) {
		iwx_check_rfkill(sc);
		iwx_task_add(sc->sc_systq, &sc->init_task);
		goto out_ena;
	}

	if (r1 & IWX_CSR_INT_BIT_SW_ERR) {
		if (ifp->if_flags & IFF_DEBUG) {
			iwx_nic_error(sc);
			iwx_dump_driver_status(sc);
		}
		printf("%s: fatal firmware error\n", DEVNAME(sc));
		if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0)
			iwx_task_add(sc->sc_systq, &sc->init_task);
		return;

	}

	if (r1 & IWX_CSR_INT_BIT_HW_ERR) {
		printf("%s: hardware error, stopping device \n", DEVNAME(sc));
		if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0) {
			sc->sc_flags |= IWX_FLAG_HW_ERR;
			iwx_task_add(sc->sc_systq, &sc->init_task);
		}
		return;
	}

	/* firmware chunk loaded */
	if (r1 & IWX_CSR_INT_BIT_FH_TX) {
		IWX_WRITE(sc, IWX_CSR_FH_INT_STATUS, IWX_CSR_FH_INT_TX_MASK);

		sc->sc_fw_chunk_done = 1;
		wakeup(&sc->sc_fw);
	}

	if (r1 & (IWX_CSR_INT_BIT_FH_RX | IWX_CSR_INT_BIT_SW_RX |
	    IWX_CSR_INT_BIT_RX_PERIODIC)) {
		if (r1 & (IWX_CSR_INT_BIT_FH_RX | IWX_CSR_INT_BIT_SW_RX)) {
			IWX_WRITE(sc, IWX_CSR_FH_INT_STATUS, IWX_CSR_FH_INT_RX_MASK);
		}
		if (r1 & IWX_CSR_INT_BIT_RX_PERIODIC) {
			IWX_WRITE(sc, IWX_CSR_INT, IWX_CSR_INT_BIT_RX_PERIODIC);
		}

		/* Disable periodic interrupt; we use it as just a one-shot. */
		IWX_WRITE_1(sc, IWX_CSR_INT_PERIODIC_REG, IWX_CSR_INT_PERIODIC_DIS);

		/*
		 * Enable periodic interrupt in 8 msec only if we received
		 * real RX interrupt (instead of just periodic int), to catch
		 * any dangling Rx interrupt.  If it was just the periodic
		 * interrupt, there was no dangling Rx activity, and no need
		 * to extend the periodic interrupt; one-shot is enough.
		 */
		if (r1 & (IWX_CSR_INT_BIT_FH_RX | IWX_CSR_INT_BIT_SW_RX))
			IWX_WRITE_1(sc, IWX_CSR_INT_PERIODIC_REG,
			    IWX_CSR_INT_PERIODIC_ENA);

		iwx_notif_intr(sc);
	}

 out_ena:
	iwx_restore_interrupts(sc);
}

static void
iwx_softintr_msix(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	uint32_t inta_fh, inta_hw;
	int vector = 0;

	inta_fh = IWX_READ(sc, IWX_CSR_MSIX_FH_INT_CAUSES_AD);
	inta_hw = IWX_READ(sc, IWX_CSR_MSIX_HW_INT_CAUSES_AD);
	if (inta_fh == 0xffffffff && inta_hw == 0xffffffff)
		return; /* Hardware gone! */
	IWX_WRITE(sc, IWX_CSR_MSIX_FH_INT_CAUSES_AD, inta_fh);
	IWX_WRITE(sc, IWX_CSR_MSIX_HW_INT_CAUSES_AD, inta_hw);
	inta_fh &= sc->sc_fh_mask;
	inta_hw &= sc->sc_hw_mask;

	if (inta_fh & IWX_MSIX_FH_INT_CAUSES_Q0 ||
	    inta_fh & IWX_MSIX_FH_INT_CAUSES_Q1) {
		iwx_notif_intr(sc);
	}

	/* firmware chunk loaded */
	if (inta_fh & IWX_MSIX_FH_INT_CAUSES_D2S_CH0_NUM) {
		sc->sc_fw_chunk_done = 1;
		wakeup(&sc->sc_fw);
	}

	if (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_TOP_FATAL_ERR) {
		printf("%s: fatal hardware error\n", DEVNAME(sc));
		if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0)
			iwx_task_add(sc->sc_systq, &sc->init_task);
		goto out;
	}

	if ((inta_fh & IWX_MSIX_FH_INT_CAUSES_FH_ERR) ||
	    (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR) ||
	    (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_V2)) {
		if (ifp->if_flags & IFF_DEBUG) {
			iwx_nic_error(sc);
			iwx_dump_driver_status(sc);
		}
		printf("%s: fatal firmware error\n", DEVNAME(sc));
		if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0)
			iwx_task_add(sc->sc_systq, &sc->init_task);
		goto out;
	}

	if (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_RF_KILL) {
		iwx_check_rfkill(sc);
		iwx_task_add(sc->sc_systq, &sc->init_task);
	}

	if (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR) {
		printf("%s: hardware error, stopping device \n", DEVNAME(sc));
		if ((sc->sc_flags & IWX_FLAG_SHUTDOWN) == 0) {
			sc->sc_flags |= IWX_FLAG_HW_ERR;
			iwx_task_add(sc->sc_systq, &sc->init_task);
		}
		goto out;
	}

	if (inta_hw & IWX_MSIX_HW_INT_CAUSES_REG_ALIVE) {
		int i;

		/* Firmware has now configured the RFH. */
		for (i = 0; i < IWX_RX_MQ_RING_COUNT; i++)
			iwx_update_rx_desc(sc, &sc->rxq, i);
		if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ) {
			IWX_WRITE(sc, IWX_HBUS_TARG_WRPTR,
			    8 | IWX_HBUS_TARG_WRPTR_RX_Q(0));
		} else
			IWX_WRITE(sc, IWX_RFH_Q0_FRBDCB_WIDX_TRG, 8);
	}

out:
	/*
	 * Before sending the interrupt the HW disables it to prevent
	 * a nested interrupt. This is done by writing 1 to the corresponding
	 * bit in the mask register. After handling the interrupt, it should be
	 * re-enabled by clearing this bit. This register is defined as
	 * write 1 clear (W1C) register, meaning that it's being clear
	 * by writing 1 to the bit.
	 */
	IWX_WRITE(sc, IWX_CSR_MSIX_AUTOMASK_ST_AD, 1 << vector);
}

static void
iwx_softintr(void *arg)
{
	struct iwx_softc *sc = arg;

	if (sc->sc_msix)
		iwx_softintr_msix(sc);
	else
		iwx_softintr_legacy(sc);
}

static const pci_product_id_t iwx_devices[] = {
	IWX_PCI_PRODUCT_WL_22500_1,
	IWX_PCI_PRODUCT_WL_22500_2,
	IWX_PCI_PRODUCT_WL_22500_3,
	IWX_PCI_PRODUCT_WL_22500_4,
	IWX_PCI_PRODUCT_WL_22500_5,
	IWX_PCI_PRODUCT_WL_22500_6,
	IWX_PCI_PRODUCT_WL_22500_7,
	IWX_PCI_PRODUCT_WL_22500_8,
	IWX_PCI_PRODUCT_WL_22500_9,
	IWX_PCI_PRODUCT_WL_22500_10,
	IWX_PCI_PRODUCT_WL_22500_11,
	IWX_PCI_PRODUCT_WL_22500_12,
	IWX_PCI_PRODUCT_WL_22500_13,
	IWX_PCI_PRODUCT_WL_22500_14,
	IWX_PCI_PRODUCT_WL_22500_15,
	IWX_PCI_PRODUCT_WL_22500_16,
	IWX_PCI_PRODUCT_WL_22500_17,
	IWX_PCI_PRODUCT_WL_22500_18,
};

static int
iwx_match(device_t parent, cfdata_t match __unused, void *aux)
{
	struct pci_attach_args *pa = aux;
	pcireg_t memtype;
	bus_space_tag_t st;
	bus_space_handle_t sh;
	bus_size_t sz;
	uint32_t rf_id;
	size_t i;
	
	if (PCI_VENDOR(pa->pa_id) != PCI_VENDOR_INTEL)
		return 0;

	for (i = 0; i < nitems(iwx_devices); i++)
		if (PCI_PRODUCT(pa->pa_id) == iwx_devices[i])
			break;
	if (i == nitems(iwx_devices))
		return 0;

	if (PCI_PRODUCT(pa->pa_id) != IWX_PCI_PRODUCT_WL_22500_18)
		return 1;

	/*
	 * Only match on BZ devices with wifi 6e RF-type GF.
	 * We do not support wifi 7 BZ devices.
	 */

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);
	if (pci_mapreg_map(pa, PCI_MAPREG_START, memtype, 0,
	    &st, &sh, NULL, &sz)) {
		printf("%s: can't map mem space\n", __func__);
		return 0;
	}

	rf_id = bus_space_read_4(st, sh, IWX_CSR_HW_RF_ID);

	bus_space_unmap(st, sh, sz);

	if (IWX_CSR_HW_RFID_TYPE(rf_id) == IWX_CFG_RF_TYPE_GF)
		return 1;

	return 0;
}

/*
 * The device info table below contains device-specific config overrides.
 * The most important parameter derived from this table is the name of the
 * firmware image to load.
 *
 * The Linux iwlwifi driver uses an "old" and a "new" device info table.
 * The "old" table matches devices based on PCI vendor/product IDs only.
 * The "new" table extends this with various device parameters derived
 * from MAC type, and RF type.
 *
 * In iwlwifi "old" and "new" tables share the same array, where "old"
 * entries contain dummy values for data defined only for "new" entries.
 * As of 2022, Linux developers are still in the process of moving entries
 * from "old" to "new" style and it looks like this effort has stalled in
 * in some work-in-progress state for quite a while. Linux commits moving
 * entries from "old" to "new" have at times been reverted due to regressions.
 * Part of this complexity comes from iwlwifi supporting both iwm(4) and iwx(4)
 * devices in the same driver.
 *
 * Our table below contains mostly "new" entries declared in iwlwifi
 * with the _IWL_DEV_INFO() macro (with a leading underscore).
 * Other devices are matched based on PCI vendor/product ID as usual,
 * unless matching specific PCI subsystem vendor/product IDs is required.
 *
 * Some "old"-style entries are required to identify the firmware image to use.
 * Others might be used to print a specific marketing name into Linux dmesg,
 * but we can't be sure whether the corresponding devices would be matched
 * correctly in the absence of their entries. So we include them just in case.
 */

struct iwx_dev_info {
	uint16_t device;
	uint16_t subdevice;
	uint16_t mac_type;
	uint16_t rf_type;
	uint8_t mac_step;
	uint8_t rf_id;
	uint8_t no_160;
	uint8_t cores;
	uint8_t cdb;
	uint8_t jacket;
	const struct iwx_device_cfg *cfg;
};

#define _IWX_DEV_INFO(_device, _subdevice, _mac_type, _mac_step, _rf_type, \
		      _rf_id, _no_160, _cores, _cdb, _jacket, _cfg) \
	{ .device = (_device), .subdevice = (_subdevice), .cfg = &(_cfg),  \
	  .mac_type = _mac_type, .rf_type = _rf_type,	   \
	  .no_160 = _no_160, .cores = _cores, .rf_id = _rf_id,		   \
	  .mac_step = _mac_step, .cdb = _cdb, .jacket = _jacket }

#define IWX_DEV_INFO(_device, _subdevice, _cfg) \
	_IWX_DEV_INFO(_device, _subdevice, IWX_CFG_ANY, IWX_CFG_ANY,	   \
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_ANY,  \
		      IWX_CFG_ANY, IWX_CFG_ANY, _cfg)

/*
 * When adding entries to this table keep in mind that entries must
 * be listed in the same order as in the Linux driver. Code walks this
 * table backwards and uses the first matching entry it finds.
 */
static const struct iwx_dev_info iwx_dev_info_table[] = {
	/* So with HR */
	IWX_DEV_INFO(0x2725, 0x0090, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x2725, 0x0020, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x2020, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x0024, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x0310, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x0510, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x0A10, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0xE020, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0xE024, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x4020, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x6020, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x6024, iwx_2ax_cfg_ty_gf_a0),
	IWX_DEV_INFO(0x2725, 0x1673, iwx_2ax_cfg_ty_gf_a0), /* killer_1675w */
	IWX_DEV_INFO(0x2725, 0x1674, iwx_2ax_cfg_ty_gf_a0), /* killer_1675x */
	IWX_DEV_INFO(0x51f0, 0x1691, iwx_2ax_cfg_so_gf4_a0), /* killer_1690s */
	IWX_DEV_INFO(0x51f0, 0x1692, iwx_2ax_cfg_so_gf4_a0), /* killer_1690i */
	IWX_DEV_INFO(0x51f1, 0x1691, iwx_2ax_cfg_so_gf4_a0),
	IWX_DEV_INFO(0x51f1, 0x1692, iwx_2ax_cfg_so_gf4_a0),
	IWX_DEV_INFO(0x54f0, 0x1691, iwx_2ax_cfg_so_gf4_a0), /* killer_1690s */
	IWX_DEV_INFO(0x54f0, 0x1692, iwx_2ax_cfg_so_gf4_a0), /* killer_1690i */
	IWX_DEV_INFO(0x7a70, 0x0090, iwx_2ax_cfg_so_gf_a0_long),
	IWX_DEV_INFO(0x7a70, 0x0098, iwx_2ax_cfg_so_gf_a0_long),
	IWX_DEV_INFO(0x7a70, 0x00b0, iwx_2ax_cfg_so_gf4_a0_long),
	IWX_DEV_INFO(0x7a70, 0x0310, iwx_2ax_cfg_so_gf_a0_long),
	IWX_DEV_INFO(0x7a70, 0x0510, iwx_2ax_cfg_so_gf_a0_long),
	IWX_DEV_INFO(0x7a70, 0x0a10, iwx_2ax_cfg_so_gf_a0_long),
	IWX_DEV_INFO(0x7af0, 0x0090, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x7af0, 0x0098, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x7af0, 0x00b0, iwx_2ax_cfg_so_gf4_a0),
	IWX_DEV_INFO(0x7a70, 0x1691, iwx_2ax_cfg_so_gf4_a0), /* killer_1690s */
	IWX_DEV_INFO(0x7a70, 0x1692, iwx_2ax_cfg_so_gf4_a0), /* killer_1690i */
	IWX_DEV_INFO(0x7af0, 0x0310, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x7af0, 0x0510, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x7af0, 0x0a10, iwx_2ax_cfg_so_gf_a0),
	IWX_DEV_INFO(0x7f70, 0x1691, iwx_2ax_cfg_so_gf4_a0), /* killer_1690s */
	IWX_DEV_INFO(0x7f70, 0x1692, iwx_2ax_cfg_so_gf4_a0), /* killer_1690i */

	/* So with GF2 */
	IWX_DEV_INFO(0x2726, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x2726, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */
	IWX_DEV_INFO(0x51f0, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x51f0, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */
	IWX_DEV_INFO(0x54f0, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x54f0, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */
	IWX_DEV_INFO(0x7a70, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x7a70, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */
	IWX_DEV_INFO(0x7af0, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x7af0, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */
	IWX_DEV_INFO(0x7f70, 0x1671, iwx_2ax_cfg_so_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x7f70, 0x1672, iwx_2ax_cfg_so_gf_a0), /* killer_1675i */

	/* MA with GF2 */
	IWX_DEV_INFO(0x7e40, 0x1671, iwx_cfg_ma_b0_gf_a0), /* killer_1675s */
	IWX_DEV_INFO(0x7e40, 0x1672, iwx_cfg_ma_b0_gf_a0), /* killer_1675i */

	/* Qu with Jf, C step */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* 9461_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* iwl9461 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* 9462_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* 9462 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* 9560_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_qu_c0_jf_b0_cfg), /* 9560 */
	_IWX_DEV_INFO(IWX_CFG_ANY, 0x1551,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY,
		      iwx_9560_qu_c0_jf_b0_cfg), /* 9560_killer_1550s */
	_IWX_DEV_INFO(IWX_CFG_ANY, 0x1552,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY,
		      iwx_9560_qu_c0_jf_b0_cfg), /* 9560_killer_1550i */

	/* QuZ with Jf */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_quz_a0_jf_b0_cfg), /* 9461_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_quz_a0_jf_b0_cfg), /* 9461 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_quz_a0_jf_b0_cfg), /* 9462_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_9560_quz_a0_jf_b0_cfg), /* 9462 */
	_IWX_DEV_INFO(IWX_CFG_ANY, 0x1551,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY,
		      iwx_9560_quz_a0_jf_b0_cfg), /* killer_1550s */
	_IWX_DEV_INFO(IWX_CFG_ANY, 0x1552,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY,
		      iwx_9560_quz_a0_jf_b0_cfg), /* 9560_killer_1550i */

	/* Qu with Hr, B step */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_B_STEP,
		      IWX_CFG_RF_TYPE_HR1, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_qu_b0_hr1_b0), /* AX101 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_B_STEP,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_qu_b0_hr_b0), /* AX203 */

	/* Qu with Hr, C step */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_HR1, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_qu_c0_hr1_b0), /* AX101 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_qu_c0_hr_b0), /* AX203 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QU, IWX_SILICON_C_STEP,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_qu_c0_hr_b0), /* AX201 */

	/* QuZ with Hr */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR1, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_quz_a0_hr1_b0), /* AX101 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_QUZ, IWX_SILICON_B_STEP,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_quz_a0_hr_b0), /* AX203 */

	/* SoF with JF2 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9560_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9560 */

	/* SoF with JF */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9461_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9462_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9461_name */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9462 */

	/* So with Hr */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* AX203 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR1, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* ax101 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* ax201 */

	/* So-F with Hr */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* AX203 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR1, IWX_CFG_ANY,
		      IWX_CFG_NO_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* AX101 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_cfg_so_a0_hr_b0), /* AX201 */

	/* So-F with GF */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_2ax_cfg_so_gf_a0), /* AX211 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SOF, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_CDB, IWX_CFG_ANY,
		      iwx_2ax_cfg_so_gf4_a0), /* AX411 */

	/* So with GF */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_NO_CDB, IWX_CFG_ANY,
		      iwx_2ax_cfg_so_gf_a0), /* AX211 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_160, IWX_CFG_ANY, IWX_CFG_CDB, IWX_CFG_ANY,
		      iwx_2ax_cfg_so_gf4_a0), /* AX411 */

	/* So with JF2 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9560_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF2, IWX_CFG_RF_ID_JF,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9560 */

	/* So with JF */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9461_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9462_160 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* iwl9461 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_SO, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_JF1, IWX_CFG_RF_ID_JF1_DIV,
		      IWX_CFG_NO_160, IWX_CFG_CORES_BT, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_2ax_cfg_so_jf_b0), /* 9462 */

	/* Ma */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_MA, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_HR2, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_cfg_ma_b0_hr_b0), /* ax201 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_MA, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_cfg_ma_b0_gf_a0), /* ax211 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_MA, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_GF, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_CDB,
		      IWX_CFG_ANY, iwx_cfg_ma_b0_gf4_a0), /* ax211 */
	_IWX_DEV_INFO(IWX_CFG_ANY, IWX_CFG_ANY,
		      IWX_CFG_MAC_TYPE_MA, IWX_CFG_ANY,
		      IWX_CFG_RF_TYPE_FM, IWX_CFG_ANY,
		      IWX_CFG_ANY, IWX_CFG_ANY, IWX_CFG_NO_CDB,
		      IWX_CFG_ANY, iwx_cfg_ma_a0_fm_a0), /* ax231 */
};

/*
 * Prepare the hardware for operation. The first time around this loads
 * the firmware in order to read the NVM (MAC address, channel map).
 */
static int
iwx_preinit(struct iwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = IC2IFP(ic);
	int err;

	err = iwx_prepare_card_hw(sc);
	if (err) {
		printf("%s: could not initialize hardware\n", DEVNAME(sc));
		return err;
	}

	if (sc->attached) {
		/* Update MAC in case the upper layers changed it. */
		if (ifp != NULL && ifp->if_sadl != NULL)
			IEEE80211_ADDR_COPY(sc->sc_ic.ic_myaddr,
			    CLLADDR(ifp->if_sadl));
		return 0;
	}

	err = iwx_start_hw(sc);
	if (err) {
		printf("%s: could not initialize hardware\n", DEVNAME(sc));
		return err;
	}

	err = iwx_run_init_mvm_ucode(sc, 1);
	iwx_stop_device(sc);
	if (err)
		return err;

	/* Print version info and MAC address on first successful fw load. */
	sc->attached = 1;
	if (sc->sc_pnvm_ver) {
		aprint_normal_dev(sc->sc_dev, "hw rev 0x%x, fw %s, pnvm %08x, "
		    "address %s\n",
		    sc->sc_hw_rev & IWX_CSR_HW_REV_TYPE_MSK,
		    sc->sc_fwver, sc->sc_pnvm_ver,
		    ether_sprintf(sc->sc_nvm.hw_addr));
	} else {
		aprint_normal_dev(sc->sc_dev, "hw rev 0x%x, fw %s, address %s\n",
		    sc->sc_hw_rev & IWX_CSR_HW_REV_TYPE_MSK,
		    sc->sc_fwver, ether_sprintf(sc->sc_nvm.hw_addr));
	}

	return 0;
}

/*
 * Complete attachment once the firmware could be loaded: attach the
 * network interface and net80211.
 */
static int
iwx_config_complete(struct iwx_softc *sc)
{
	device_t self = sc->sc_dev;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	int err;

	KASSERT(!ISSET(sc->sc_flags, IWX_FLAG_ATTACHED));

	err = iwx_preinit(sc);
	if (err)
		return err;

	ic->ic_ifp = ifp;
	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */
	ic->ic_state = IEEE80211_S_INIT;

	/*
	 * Set device capabilities. We do not claim hardware support for
	 * any cipher so that net80211 uses software crypto.
	 */
	ic->ic_caps =
	    IEEE80211_C_WPA |		/* 802.11i */
	    IEEE80211_C_MONITOR |	/* monitor mode supported */
	    IEEE80211_C_SHSLOT |	/* short slot time supported */
	    IEEE80211_C_SHPREAMBLE |	/* short preamble supported */
	    IEEE80211_C_PMGT;		/* LISPBSD: allow ifconfig powersave */

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	if (sc->sc_nvm.sku_cap_band_52GHz_enable)
		ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;

	/* IBSS channel undefined for now. */
	ic->ic_ibss_chan = &ic->ic_channels[1];

	IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_nvm.hw_addr);

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = iwx_init;
	ifp->if_stop = iwx_stop;
	ifp->if_ioctl = iwx_ioctl;
	ifp->if_start = iwx_start;
	ifp->if_watchdog = iwx_watchdog;
	IFQ_SET_READY(&ifp->if_snd);
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);

	if_initialize(ifp);
	ieee80211_ifattach(ic);
	/* Use common softint-based if_input */
	ifp->if_percpuq = if_percpuq_create(ifp);
	if_register(ifp);

	ic->ic_node_alloc = iwx_node_alloc;

	/* Override 802.11 state transition machine. */
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = iwx_newstate;

	/* XXX media locking needs revisiting */
	mutex_init(&sc->sc_media_mtx, MUTEX_DEFAULT, IPL_SOFTNET);
	ieee80211_media_init_with_lock(ic,
	    iwx_media_change, ieee80211_media_status, &sc->sc_media_mtx);

	ieee80211_announce(ic);

	iwx_radiotap_attach(sc);

	if (pmf_device_register(self, NULL, NULL))
		pmf_class_network_register(self, ifp);
	else
		aprint_error_dev(self, "couldn't establish power handler\n");

	sc->sc_flags |= IWX_FLAG_ATTACHED;

	return 0;
}

static void
iwx_attach_hook(device_t self)
{
	struct iwx_softc *sc = device_private(self);

	iwx_config_complete(sc);
}

static const struct iwx_device_cfg *
iwx_find_device_cfg(struct iwx_softc *sc)
{
	pcireg_t sreg;
	pci_product_id_t sdev_id;
	uint16_t mac_type, rf_type;
	uint8_t mac_step, cdb, jacket, rf_id, no_160, cores;
	int i;

	sreg = pci_conf_read(sc->sc_pct, sc->sc_pcitag, PCI_SUBSYS_ID_REG);
	sdev_id = PCI_PRODUCT(sreg);
	mac_type = IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev);
	mac_step = IWX_CSR_HW_REV_STEP(sc->sc_hw_rev << 2);
	rf_type = IWX_CSR_HW_RFID_TYPE(sc->sc_hw_rf_id);
	cdb = IWX_CSR_HW_RFID_IS_CDB(sc->sc_hw_rf_id);
	jacket = IWX_CSR_HW_RFID_IS_JACKET(sc->sc_hw_rf_id);

	rf_id = IWX_SUBDEVICE_RF_ID(sdev_id);
	no_160 = IWX_SUBDEVICE_NO_160(sdev_id);
	cores = IWX_SUBDEVICE_CORES(sdev_id);

	for (i = nitems(iwx_dev_info_table) - 1; i >= 0; i--) {
		const struct iwx_dev_info *dev_info = &iwx_dev_info_table[i];

		if (dev_info->device != (uint16_t)IWX_CFG_ANY &&
		    dev_info->device != sc->sc_pid)
			continue;

		if (dev_info->subdevice != (uint16_t)IWX_CFG_ANY &&
		    dev_info->subdevice != sdev_id)
			continue;

		if (dev_info->mac_type != (uint16_t)IWX_CFG_ANY &&
		    dev_info->mac_type != mac_type)
			continue;

		if (dev_info->mac_step != (uint8_t)IWX_CFG_ANY &&
		    dev_info->mac_step != mac_step)
			continue;

		if (dev_info->rf_type != (uint16_t)IWX_CFG_ANY &&
		    dev_info->rf_type != rf_type)
			continue;

		if (dev_info->cdb != (uint8_t)IWX_CFG_ANY &&
		    dev_info->cdb != cdb)
			continue;

		if (dev_info->jacket != (uint8_t)IWX_CFG_ANY &&
		    dev_info->jacket != jacket)
			continue;

		if (dev_info->rf_id != (uint8_t)IWX_CFG_ANY &&
		    dev_info->rf_id != rf_id)
			continue;

		if (dev_info->no_160 != (uint8_t)IWX_CFG_ANY &&
		    dev_info->no_160 != no_160)
			continue;

		if (dev_info->cores != (uint8_t)IWX_CFG_ANY &&
		    dev_info->cores != cores)
			continue;

		return dev_info->cfg;
	}

	return NULL;
}

static void
iwx_get_crf_id(struct iwx_softc *sc)
{
	uint32_t val = 0;
	uint8_t step = 0;

	/* Enable access to peripheral registers */
	val = iwx_read_umac_prph_unlocked(sc, IWX_WFPM_CTRL_REG);
	val |= IWX_WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK;
	iwx_write_umac_prph_unlocked(sc, IWX_WFPM_CTRL_REG, val);

	/* Read crf info */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		sc->sc_hw_crf_id = iwx_read_prph_unlocked(sc, IWX_SD_REG_VER_GEN2);
	else
		sc->sc_hw_crf_id = iwx_read_prph_unlocked(sc, IWX_SD_REG_VER);

	/* Read cnv info */
	sc->sc_hw_cnv_id = iwx_read_prph_unlocked(sc, IWX_CNVI_AUX_MISC_CHIP);

	/* For BZ-W, take B step also when A step is indicated */
	if (IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) == IWX_CFG_MAC_TYPE_BZ_W)
		step = IWX_SILICON_B_STEP;

	/* In BZ, the MAC step must be read from the CNVI aux register */
	if (IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) == IWX_CFG_MAC_TYPE_BZ) {
		step = IWX_CNVI_AUX_MISC_CHIP_MAC_STEP(sc->sc_hw_cnv_id);

		/* For BZ-U, take B step also when A step is indicated */
		if (IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE(sc->sc_hw_cnv_id) ==
		    IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_U &&
		    step == IWX_SILICON_A_STEP)
			step = IWX_SILICON_B_STEP;
	}

	if (IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) == IWX_CFG_MAC_TYPE_BZ ||
	    IWX_CSR_HW_REV_TYPE(sc->sc_hw_rev) == IWX_CFG_MAC_TYPE_BZ_W)
		sc->sc_hw_rev |= step;

	DPRINTF(("%s: Detected crf-id 0x%x, cnv-id 0x%x\n", DEVNAME(sc),
	    sc->sc_hw_crf_id, sc->sc_hw_cnv_id));
}

static void
iwx_attach(device_t parent, device_t self, void *aux)
{
	struct iwx_softc *sc = device_private(self);
	struct pci_attach_args *pa = aux;
	pcireg_t reg, memtype;
	struct ieee80211com *ic = &sc->sc_ic;
	char intrbuf[PCI_INTRSTR_LEN];
	const char *intrstr;
	const struct iwx_device_cfg *cfg;
	int counts[PCI_INTR_TYPE_SIZE];
	int err;
	int txq_i, i;
	size_t ctxt_info_size;

	sc->sc_dev = self;
	sc->sc_pid = PCI_PRODUCT(pa->pa_id);
	sc->sc_pct = pa->pa_pc;
	sc->sc_pcitag = pa->pa_tag;
	sc->sc_dmat = pa->pa_dmat;

	pci_aprint_devinfo(pa, NULL);

	rw_init(&sc->ioctl_rwl);

	err = pci_get_capability(sc->sc_pct, sc->sc_pcitag,
	    PCI_CAP_PCIEXPRESS, &sc->sc_cap_off, NULL);
	if (err == 0) {
		aprint_error_dev(self,
		    "PCIe capability structure not found!\n");
		return;
	}

	/*
	 * We disable the RETRY_TIMEOUT register (0x41) to keep
	 * PCI Tx retries from interfering with C3 CPU state.
	 */
	reg = pci_conf_read(sc->sc_pct, sc->sc_pcitag, 0x40);
	pci_conf_write(sc->sc_pct, sc->sc_pcitag, 0x40, reg & ~0xff00);

	/* Enable bus-mastering. */
	reg = pci_conf_read(sc->sc_pct, sc->sc_pcitag, PCI_COMMAND_STATUS_REG);
	reg |= PCI_COMMAND_MASTER_ENABLE;
	pci_conf_write(sc->sc_pct, sc->sc_pcitag, PCI_COMMAND_STATUS_REG, reg);

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);
	err = pci_mapreg_map(pa, PCI_MAPREG_START, memtype, 0,
	    &sc->sc_st, &sc->sc_sh, NULL, &sc->sc_sz);
	if (err) {
		aprint_error_dev(self, "can't map mem space\n");
		return;
	}

	/* Install interrupt handler. Prefer MSI-X, then MSI, then INTx. */
	counts[PCI_INTR_TYPE_INTX] = 1;
	counts[PCI_INTR_TYPE_MSI] = 1;
	counts[PCI_INTR_TYPE_MSIX] = 1;
	err = pci_intr_alloc(pa, &sc->sc_pihp, counts, PCI_INTR_TYPE_MSIX);
	if (err) {
		aprint_error_dev(self, "can't allocate interrupt\n");
		goto fail_unmap;
	}
	sc->sc_msix = (pci_intr_type(sc->sc_pct, sc->sc_pihp[0]) ==
	    PCI_INTR_TYPE_MSIX);
	reg = pci_conf_read(sc->sc_pct, sc->sc_pcitag, PCI_COMMAND_STATUS_REG);
	if (pci_intr_type(sc->sc_pct, sc->sc_pihp[0]) == PCI_INTR_TYPE_INTX)
		CLR(reg, PCI_COMMAND_INTERRUPT_DISABLE);
	else
		SET(reg, PCI_COMMAND_INTERRUPT_DISABLE);
	pci_conf_write(sc->sc_pct, sc->sc_pcitag, PCI_COMMAND_STATUS_REG, reg);
	intrstr = pci_intr_string(sc->sc_pct, sc->sc_pihp[0], intrbuf,
	    sizeof(intrbuf));
	sc->sc_ih = pci_intr_establish_xname(sc->sc_pct, sc->sc_pihp[0],
	    IPL_NET, sc->sc_msix ? iwx_intr_msix : iwx_intr, sc,
	    device_xname(self));
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "can't establish interrupt");
		if (intrstr != NULL)
			aprint_error(" at %s", intrstr);
		aprint_error("\n");
		goto fail_intr_release;
	}
	aprint_normal_dev(self, "interrupting at %s%s\n", intrstr,
	    sc->sc_msix ? " (MSI-X)" : "");

	sc->sc_soft_ih = softint_establish(SOFTINT_NET, iwx_softintr, sc);
	if (sc->sc_soft_ih == NULL) {
		aprint_error_dev(self, "can't establish soft interrupt\n");
		goto fail_intr;
	}

	/* Clear pending interrupts. */
	IWX_WRITE(sc, IWX_CSR_INT_MASK, 0);
	IWX_WRITE(sc, IWX_CSR_INT, ~0);
	IWX_WRITE(sc, IWX_CSR_FH_INT_STATUS, ~0);

	sc->sc_hw_rev = IWX_READ(sc, IWX_CSR_HW_REV);
	sc->sc_hw_rf_id = IWX_READ(sc, IWX_CSR_HW_RF_ID);

	/*
	 * In the 8000 HW family the format of the 4 bytes of CSR_HW_REV have
	 * changed, and now the revision step also includes bit 0-1 (no more
	 * "dash" value). To keep hw_rev backwards compatible - we'll store it
	 * in the old format.
	 */
	sc->sc_hw_rev = (sc->sc_hw_rev & 0xfff0) |
			(IWX_CSR_HW_REV_STEP(sc->sc_hw_rev << 2) << 2);

	switch (PCI_PRODUCT(pa->pa_id)) {
	case IWX_PCI_PRODUCT_WL_22500_1:
		sc->sc_fwname = IWX_CC_A_FW;
		sc->sc_device_family = IWX_DEVICE_FAMILY_22000;
		sc->sc_integrated = 0;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_NONE;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 0;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		break;
	case IWX_PCI_PRODUCT_WL_22500_2:
	case IWX_PCI_PRODUCT_WL_22500_5:
		/* These devices should be QuZ only. */
		if (sc->sc_hw_rev != IWX_CSR_HW_REV_TYPE_QUZ) {
			aprint_error_dev(self, "unsupported AX201 adapter\n");
			goto fail_softint;
		}
		sc->sc_fwname = IWX_QUZ_A_HR_B_FW;
		sc->sc_device_family = IWX_DEVICE_FAMILY_22000;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_200;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 500;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		break;
	case IWX_PCI_PRODUCT_WL_22500_3:
		if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QU_C0)
			sc->sc_fwname = IWX_QU_C_HR_B_FW;
		else if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QUZ) {
			uint32_t rf_id = IWX_CSR_HW_RFID_TYPE(sc->sc_hw_rf_id);
			if (rf_id == IWX_CFG_RF_TYPE_JF1 ||
			    rf_id == IWX_CFG_RF_TYPE_JF2)
				sc->sc_fwname = IWX_QUZ_A_JF_B_FW;
			else
				sc->sc_fwname = IWX_QUZ_A_HR_B_FW;
		} else
			sc->sc_fwname = IWX_QU_B_HR_B_FW;
		sc->sc_device_family = IWX_DEVICE_FAMILY_22000;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_200;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 500;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		break;
	case IWX_PCI_PRODUCT_WL_22500_4:
	case IWX_PCI_PRODUCT_WL_22500_7:
	case IWX_PCI_PRODUCT_WL_22500_8:
		if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QU_C0)
			sc->sc_fwname = IWX_QU_C_HR_B_FW;
		else if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QUZ)
			sc->sc_fwname = IWX_QUZ_A_HR_B_FW;
		else
			sc->sc_fwname = IWX_QU_B_HR_B_FW;
		sc->sc_device_family = IWX_DEVICE_FAMILY_22000;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_1820;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 1820;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		break;
	case IWX_PCI_PRODUCT_WL_22500_6:
		if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QU_C0)
			sc->sc_fwname = IWX_QU_C_HR_B_FW;
		else if (sc->sc_hw_rev == IWX_CSR_HW_REV_TYPE_QUZ)
			sc->sc_fwname = IWX_QUZ_A_HR_B_FW;
		else
			sc->sc_fwname = IWX_QU_B_HR_B_FW;
		sc->sc_device_family = IWX_DEVICE_FAMILY_22000;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500;
		sc->sc_low_latency_xtal = 1;
		sc->sc_xtal_latency = 12000;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		break;
	case IWX_PCI_PRODUCT_WL_22500_9:
	case IWX_PCI_PRODUCT_WL_22500_10:
	case IWX_PCI_PRODUCT_WL_22500_11:
	case IWX_PCI_PRODUCT_WL_22500_13:
	case IWX_PCI_PRODUCT_WL_22500_15:
	case IWX_PCI_PRODUCT_WL_22500_16:
		sc->sc_fwname = IWX_SO_A_GF_A_FW;
		sc->sc_pnvm_name = IWX_SO_A_GF_A_PNVM;
		sc->sc_device_family = IWX_DEVICE_FAMILY_AX210;
		sc->sc_integrated = 0;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_NONE;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 0;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 1;
		break;
	case IWX_PCI_PRODUCT_WL_22500_12:
	case IWX_PCI_PRODUCT_WL_22500_17:
		sc->sc_fwname = IWX_SO_A_GF_A_FW;
		sc->sc_pnvm_name = IWX_SO_A_GF_A_PNVM;
		sc->sc_device_family = IWX_DEVICE_FAMILY_AX210;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500;
		sc->sc_low_latency_xtal = 1;
		sc->sc_xtal_latency = 12000;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 0;
		sc->sc_imr_enabled = 1;
		break;
	case IWX_PCI_PRODUCT_WL_22500_14:
		sc->sc_fwname = IWX_MA_B_GF_A_FW;
		sc->sc_pnvm_name = IWX_MA_B_GF_A_PNVM;
		sc->sc_device_family = IWX_DEVICE_FAMILY_AX210;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_NONE;
		sc->sc_low_latency_xtal = 0;
		sc->sc_xtal_latency = 0;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 1;
		break;
	case IWX_PCI_PRODUCT_WL_22500_18:
		sc->sc_fwname = IWX_BZ_B_GF_A_FW;
		sc->sc_pnvm_name = IWX_BZ_B_GF_A_PNVM;
		sc->sc_device_family = IWX_DEVICE_FAMILY_BZ;
		sc->sc_integrated = 1;
		sc->sc_ltr_delay = IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500;
		sc->sc_xtal_latency = 12000;
		sc->sc_low_latency_xtal = true;
		sc->sc_tx_with_siso_diversity = 0;
		sc->sc_uhb_supported = 1;
		break;
	default:
		aprint_error_dev(self, "unknown adapter type\n");
		goto fail_softint;
	}

	cfg = iwx_find_device_cfg(sc);
	if (cfg) {
		sc->sc_fwname = cfg->fw_name;
		sc->sc_pnvm_name = cfg->pnvm_name;
		sc->sc_tx_with_siso_diversity = cfg->tx_with_siso_diversity;
		sc->sc_uhb_supported = cfg->uhb_supported;
		if (cfg->xtal_latency) {
			sc->sc_xtal_latency = cfg->xtal_latency;
			sc->sc_low_latency_xtal = cfg->low_latency_xtal;
		}
	}

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_BZ)
		sc->mac_addr_from_csr = 0x30;
	else
		sc->mac_addr_from_csr = 0x380;

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		sc->sc_umac_prph_offset = 0x300000;
		sc->max_tfd_queue_size = IWX_TFD_QUEUE_SIZE_MAX_GEN3;
	} else
		sc->max_tfd_queue_size = IWX_TFD_QUEUE_SIZE_MAX;

	if (iwx_apm_init(sc) == 0 && iwx_nic_lock(sc)) {
		iwx_get_crf_id(sc);
		iwx_nic_unlock(sc);
		iwx_apm_stop(sc);
	}

	aprint_verbose_dev(self, "hw_rev 0x%x rf_id 0x%x firmware %s\n",
	    sc->sc_hw_rev, sc->sc_hw_rf_id, sc->sc_fwname);

	/* Allocate DMA memory for loading firmware. */
	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210)
		ctxt_info_size = sizeof(struct iwx_context_info_gen3);
	else
		ctxt_info_size = sizeof(struct iwx_context_info);
	err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->ctxt_info_dma,
	    ctxt_info_size, 0);
	if (err) {
		aprint_error_dev(self,
		    "could not allocate memory for loading firmware\n");
		goto fail_softint;
	}

	if (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210) {
		err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->prph_scratch_dma,
		    sizeof(struct iwx_prph_scratch), 0);
		if (err) {
			aprint_error_dev(self,
			    "could not allocate prph scratch memory\n");
			goto fail1;
		}

		/*
		 * Allocate prph information. The driver doesn't use this.
		 * We use the second half of this page to give the device
		 * some dummy TR/CR tail pointers - which shouldn't be
		 * necessary as we don't use this, but the hardware still
		 * reads/writes there and we can't let it go do that with
		 * a NULL pointer.
		 */
		KASSERT(sizeof(struct iwx_prph_info) < PAGE_SIZE / 2);
		err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->prph_info_dma,
		    PAGE_SIZE, 0);
		if (err) {
			aprint_error_dev(self,
			    "could not allocate prph info memory\n");
			goto fail1;
		}
	}

	/* Allocate interrupt cause table (ICT).*/
	err = iwx_dma_contig_alloc(sc->sc_dmat, &sc->ict_dma,
	    IWX_ICT_SIZE, 1<<IWX_ICT_PADDR_SHIFT);
	if (err) {
		aprint_error_dev(self, "could not allocate ICT table\n");
		goto fail1;
	}

	for (txq_i = 0; txq_i < nitems(sc->txq); txq_i++) {
		err = iwx_alloc_tx_ring(sc, &sc->txq[txq_i], txq_i);
		if (err) {
			aprint_error_dev(self,
			    "could not allocate TX ring %d\n", txq_i);
			goto fail4;
		}
	}

	err = iwx_alloc_rx_ring(sc, &sc->rxq);
	if (err) {
		aprint_error_dev(self, "could not allocate RX ring\n");
		goto fail4;
	}

	sc->sc_systq = iwx_taskq_create("iwxtq");
	sc->sc_nswq = iwx_taskq_create("iwxns");
	if (sc->sc_systq == NULL || sc->sc_nswq == NULL) {
		aprint_error_dev(self, "could not create task queues\n");
		goto fail5;
	}

	iwx_task_set(&sc->init_task, iwx_init_task, sc);
	iwx_task_set(&sc->newstate_task, iwx_newstate_task, sc);

	for (i = 0; i < nitems(sc->sc_phyctxt); i++) {
		sc->sc_phyctxt[i].id = i;
		sc->sc_phyctxt[i].sco = 0;
		sc->sc_phyctxt[i].vht_chan_width = 0;
	}

	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	sc->ns_nstate = IEEE80211_S_INIT;

	/*
	 * We cannot read the MAC address without loading the
	 * firmware from disk. Postpone until mountroot is done.
	 */
	config_mountroot(self, iwx_attach_hook);

	return;

fail5:	iwx_taskq_destroy(sc->sc_systq);
	iwx_taskq_destroy(sc->sc_nswq);
	sc->sc_systq = sc->sc_nswq = NULL;
	iwx_free_rx_ring(sc, &sc->rxq);
fail4:	while (--txq_i >= 0)
		iwx_free_tx_ring(sc, &sc->txq[txq_i]);
	if (sc->ict_dma.vaddr != NULL)
		iwx_dma_contig_free(&sc->ict_dma);

fail1:	iwx_dma_contig_free(&sc->ctxt_info_dma);
	iwx_dma_contig_free(&sc->prph_scratch_dma);
	iwx_dma_contig_free(&sc->prph_info_dma);
fail_softint:
	softint_disestablish(sc->sc_soft_ih);
	sc->sc_soft_ih = NULL;
fail_intr:
	pci_intr_disestablish(sc->sc_pct, sc->sc_ih);
	sc->sc_ih = NULL;
fail_intr_release:
	pci_intr_release(sc->sc_pct, sc->sc_pihp, 1);
	sc->sc_pihp = NULL;
fail_unmap:
	bus_space_unmap(sc->sc_st, sc->sc_sh, sc->sc_sz);
	sc->sc_sz = 0;
	rw_destroy(&sc->ioctl_rwl);
}

static int
iwx_detach(device_t self, int flags)
{
	struct iwx_softc *sc = device_private(self);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	int i;

	if (sc->sc_sz == 0)
		return 0;	/* attach failed early */

	if (ISSET(sc->sc_flags, IWX_FLAG_ATTACHED)) {
		pmf_device_deregister(self);

		rw_enter(&sc->ioctl_rwl, RW_WRITER);
		if (ifp->if_flags & IFF_RUNNING)
			iwx_stop_locked(ifp);
		ifp->if_flags &= ~IFF_UP;
		rw_exit(&sc->ioctl_rwl);

		bpf_detach(ifp);
		ieee80211_ifdetach(ic);
		if_detach(ifp);
		mutex_destroy(&sc->sc_media_mtx);
		sc->sc_flags &= ~IWX_FLAG_ATTACHED;
	}

	/* Make sure no tasks are left; tear down the task threads. */
	if (sc->sc_systq != NULL) {
		iwx_task_del(sc->sc_systq, &sc->init_task);
		iwx_taskq_destroy(sc->sc_systq);
		sc->sc_systq = NULL;
	}
	if (sc->sc_nswq != NULL) {
		iwx_task_del(sc->sc_nswq, &sc->newstate_task);
		iwx_taskq_destroy(sc->sc_nswq);
		sc->sc_nswq = NULL;
	}

	/*
	 * The device is already stopped (by iwx_stop_locked() above or
	 * at the end of iwx_preinit()); just make sure it stays quiet.
	 */
	iwx_disable_interrupts(sc);

	if (sc->sc_soft_ih != NULL) {
		softint_disestablish(sc->sc_soft_ih);
		sc->sc_soft_ih = NULL;
	}
	if (sc->sc_ih != NULL) {
		pci_intr_disestablish(sc->sc_pct, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_pihp != NULL) {
		pci_intr_release(sc->sc_pct, sc->sc_pihp, 1);
		sc->sc_pihp = NULL;
	}

	iwx_free_rx_ring(sc, &sc->rxq);
	for (i = 0; i < nitems(sc->txq); i++)
		iwx_free_tx_ring(sc, &sc->txq[i]);
	for (i = 0; i < nitems(sc->sc_cmd_resp_pkt); i++) {
		if (sc->sc_cmd_resp_pkt[i] != NULL)
			kmem_intr_free(sc->sc_cmd_resp_pkt[i],
			    sc->sc_cmd_resp_len[i]);
		sc->sc_cmd_resp_pkt[i] = NULL;
	}
	iwx_dma_contig_free(&sc->ict_dma);
	iwx_dma_contig_free(&sc->ctxt_info_dma);
	iwx_dma_contig_free(&sc->prph_scratch_dma);
	iwx_dma_contig_free(&sc->prph_info_dma);
	iwx_dma_contig_free(&sc->iml_dma);
	iwx_dma_contig_free(&sc->fw_mon);
	iwx_ctxt_info_free_fw_img(sc);
	iwx_ctxt_info_free_paging(sc);
	iwx_fw_info_free(&sc->sc_fw);
	sc->sc_fw.fw_status = IWX_FW_STATUS_NONE;

	bus_space_unmap(sc->sc_st, sc->sc_sh, sc->sc_sz);
	sc->sc_sz = 0;
	rw_destroy(&sc->ioctl_rwl);

	return 0;
}

static void
iwx_radiotap_attach(struct iwx_softc *sc)
{
	struct ifnet *ifp = IC2IFP(&sc->sc_ic);

	bpf_attach2(ifp, DLT_IEEE802_11_RADIO,
	    sizeof (struct ieee80211_frame) + IEEE80211_RADIOTAP_HDRLEN,
	    &sc->sc_drvbpf);

	sc->sc_rxtap_len = sizeof sc->sc_rxtapu;
	sc->sc_rxtap.wr_ihdr.it_len = htole16(sc->sc_rxtap_len);
	sc->sc_rxtap.wr_ihdr.it_present = htole32(IWX_RX_RADIOTAP_PRESENT);

	sc->sc_txtap_len = sizeof sc->sc_txtapu;
	sc->sc_txtap.wt_ihdr.it_len = htole16(sc->sc_txtap_len);
	sc->sc_txtap.wt_ihdr.it_present = htole32(IWX_TX_RADIOTAP_PRESENT);
}

static void
iwx_init_task(void *arg1)
{
	struct iwx_softc *sc = arg1;
	struct ifnet *ifp = IC2IFP(&sc->sc_ic);
	int s = splnet();
	int generation = sc->sc_generation;
	int fatal = (sc->sc_flags & (IWX_FLAG_HW_ERR | IWX_FLAG_RFKILL));

	if (!ISSET(sc->sc_flags, IWX_FLAG_ATTACHED)) {
		splx(s);
		return;
	}

	rw_enter(&sc->ioctl_rwl, RW_WRITER);
	if (generation != sc->sc_generation) {
		rw_exit(&sc->ioctl_rwl);
		splx(s);
		return;
	}

	if (ifp->if_flags & IFF_RUNNING)
		iwx_stop_locked(ifp);
	else
		sc->sc_flags &= ~IWX_FLAG_HW_ERR;

	if (!fatal && (ifp->if_flags & (IFF_UP | IFF_RUNNING)) == IFF_UP)
		iwx_init_locked(ifp);

	rw_exit(&sc->ioctl_rwl);
	splx(s);
}

CFATTACH_DECL_NEW(iwx, sizeof(struct iwx_softc), iwx_match, iwx_attach,
	iwx_detach, NULL);

#ifdef IWX_DEBUG
SYSCTL_SETUP(sysctl_iwx, "sysctl iwx(4) subtree setup")
{
	const struct sysctlnode *rnode;
	const struct sysctlnode *cnode;
	int rc;

	if ((rc = sysctl_createv(clog, 0, NULL, &rnode,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "iwx",
	    SYSCTL_DESCR("iwx global controls"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	/* control debugging printfs */
	if ((rc = sysctl_createv(clog, 0, &rnode, &cnode,
	    CTLFLAG_PERMANENT|CTLFLAG_READWRITE, CTLTYPE_INT,
	    "debug", SYSCTL_DESCR("Enable debugging output"),
	    NULL, 0, &iwx_debug, 0, CTL_CREATE, CTL_EOL)) != 0)
		goto err;

	return;

 err:
	aprint_error("%s: sysctl_createv failed (rc = %d)\n", __func__, rc);
}
#endif /* IWX_DEBUG */

MODULE(MODULE_CLASS_DRIVER, if_iwx, "pci");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
if_iwx_modcmd(modcmd_t cmd, void *opaque)
{
	int error = 0;

#ifdef _MODULE
	switch (cmd) {
	case MODULE_CMD_INIT:
		error = config_init_component(cfdriver_ioconf_if_iwx,
		    cfattach_ioconf_if_iwx, cfdata_ioconf_if_iwx);
		break;
	case MODULE_CMD_FINI:
		error = config_fini_component(cfdriver_ioconf_if_iwx,
		    cfattach_ioconf_if_iwx, cfdata_ioconf_if_iwx);
		break;
	default:
		error = ENOTTY;
		break;
	}
#endif

	return error;
}
