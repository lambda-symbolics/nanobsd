/*	$NetBSD: intel_bw.c,v 1.2 2021/12/18 23:45:29 riastradh Exp $	*/

// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: intel_bw.c,v 1.2 2021/12/18 23:45:29 riastradh Exp $");

#include <drm/drm_atomic_state_helper.h>

#include "intel_bw.h"
#include "intel_display_types.h"
#include "intel_sideband.h"
#include "intel_atomic.h"
#include "intel_pm.h"


/* Parameters for Qclk Geyserville (QGV) */
struct intel_qgv_point {
	u16 dclk, t_rp, t_rdpre, t_rc, t_ras, t_rcd;
};

struct intel_qgv_info {
	struct intel_qgv_point points[I915_NUM_QGV_POINTS];
	u8 num_points;
	u8 num_channels;
	u8 t_bl;
	enum intel_dram_type dram_type;
};

static int icl_pcode_read_mem_global_info(struct drm_i915_private *dev_priv,
					  struct intel_qgv_info *qi)
{
	u32 val = 0;
	int ret;

	ret = sandybridge_pcode_read(dev_priv,
				     ICL_PCODE_MEM_SUBSYSYSTEM_INFO |
				     ICL_PCODE_MEM_SS_READ_GLOBAL_INFO,
				     &val, NULL);
	dev_priv->lispbsd_sagv.global_ret = ret;
	dev_priv->lispbsd_sagv.global_raw = val;
	if (ret)
		return ret;

	if (IS_GEN(dev_priv, 12)) {
		switch (val & 0xf) {
		case 0:
			qi->dram_type = INTEL_DRAM_DDR4;
			break;
		case 3:
			qi->dram_type = INTEL_DRAM_LPDDR4;
			break;
		case 4:
			qi->dram_type = INTEL_DRAM_DDR3;
			break;
		case 5:
			qi->dram_type = INTEL_DRAM_LPDDR3;
			break;
		default:
			MISSING_CASE(val & 0xf);
			break;
		}
	} else if (IS_GEN(dev_priv, 11)) {
		switch (val & 0xf) {
		case 0:
			qi->dram_type = INTEL_DRAM_DDR4;
			break;
		case 1:
			qi->dram_type = INTEL_DRAM_DDR3;
			break;
		case 2:
			qi->dram_type = INTEL_DRAM_LPDDR3;
			break;
		case 3:
			qi->dram_type = INTEL_DRAM_LPDDR4;
			break;
		default:
			MISSING_CASE(val & 0xf);
			break;
		}
	} else {
		MISSING_CASE(INTEL_GEN(dev_priv));
		qi->dram_type = INTEL_DRAM_LPDDR3; /* Conservative default */
	}

	qi->num_channels = (val & 0xf0) >> 4;
	qi->num_points = (val & 0xf00) >> 8;

	if (IS_GEN(dev_priv, 12))
		qi->t_bl = qi->dram_type == INTEL_DRAM_DDR4 ? 4 : 16;
	else if (IS_GEN(dev_priv, 11))
		qi->t_bl = qi->dram_type == INTEL_DRAM_DDR4 ? 4 : 8;

	return 0;
}

static int icl_pcode_read_qgv_point_info(struct drm_i915_private *dev_priv,
					 struct intel_qgv_point *sp,
					 int point)
{
	u32 val = 0, val2 = 0;
	int ret;

	ret = sandybridge_pcode_read(dev_priv,
				     ICL_PCODE_MEM_SUBSYSYSTEM_INFO |
				     ICL_PCODE_MEM_SS_READ_QGV_POINT_INFO(point),
				     &val, &val2);
	if (point < I915_NUM_QGV_POINTS) {
		dev_priv->lispbsd_sagv.qgv_ret[point] = ret;
		dev_priv->lispbsd_sagv.qgv_raw[point][0] = val;
		dev_priv->lispbsd_sagv.qgv_raw[point][1] = val2;
	}
	if (ret)
		return ret;

	sp->dclk = val & 0xffff;
	sp->t_rp = (val & 0xff0000) >> 16;
	sp->t_rcd = (val & 0xff000000) >> 24;

	sp->t_rdpre = val2 & 0xff;
	sp->t_ras = (val2 & 0xff00) >> 8;

	sp->t_rc = sp->t_rp + sp->t_ras;

	return 0;
}

int icl_pcode_restrict_qgv_points(struct drm_i915_private *dev_priv,
				  u32 points_mask)
{
	int ret;

	/* bspec says to keep retrying for at least 1 ms */
	ret = skl_pcode_request(dev_priv, ICL_PCODE_SAGV_DE_MEM_SS_CONFIG,
				points_mask,
				ICL_PCODE_POINTS_RESTRICTED_MASK,
				ICL_PCODE_POINTS_RESTRICTED,
				1);

	/* LISPBSD: hw.i915sagv */
	dev_priv->lispbsd_sagv.mask_written = true;
	dev_priv->lispbsd_sagv.last_mask = points_mask;
	dev_priv->lispbsd_sagv.last_ret = ret;
	dev_priv->lispbsd_sagv.writes++;
	if (ret < 0)
		dev_priv->lispbsd_sagv.errors++;
	DRM_DEBUG_KMS("LISPBSD SAGV: QGV mask 0x%x -> %d\n", points_mask, ret);

	if (ret < 0) {
		drm_err(&dev_priv->drm, "Failed to disable qgv points (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int icl_get_qgv_points(struct drm_i915_private *dev_priv,
			      struct intel_qgv_info *qi)
{
	int i, ret;

	ret = icl_pcode_read_mem_global_info(dev_priv, qi);
	if (ret)
		return ret;

	if (WARN_ON(qi->num_points > ARRAY_SIZE(qi->points)))
		qi->num_points = ARRAY_SIZE(qi->points);

	for (i = 0; i < qi->num_points; i++) {
		struct intel_qgv_point *sp = &qi->points[i];

		ret = icl_pcode_read_qgv_point_info(dev_priv, sp, i);
		if (ret)
			return ret;

		DRM_DEBUG_KMS("QGV %d: DCLK=%d tRP=%d tRDPRE=%d tRAS=%d tRCD=%d tRC=%d\n",
			      i, sp->dclk, sp->t_rp, sp->t_rdpre, sp->t_ras,
			      sp->t_rcd, sp->t_rc);
	}

	return 0;
}

static int icl_calc_bw(int dclk, int num, int den)
{
	/* multiples of 16.666MHz (100/6) */
	return DIV_ROUND_CLOSEST(num * dclk * 100, den * 6);
}

static int icl_sagv_max_dclk(const struct intel_qgv_info *qi)
{
	u16 dclk = 0;
	int i;

	for (i = 0; i < qi->num_points; i++)
		dclk = max(dclk, qi->points[i].dclk);

	return dclk;
}

struct intel_sa_info {
	u16 displayrtids;
	u8 deburst, deprogbwlimit;
};

static const struct intel_sa_info icl_sa_info = {
	.deburst = 8,
	.deprogbwlimit = 25, /* GB/s */
	.displayrtids = 128,
};

static const struct intel_sa_info tgl_sa_info = {
	.deburst = 16,
	.deprogbwlimit = 34, /* GB/s */
	.displayrtids = 256,
};

static int icl_get_bw_info(struct drm_i915_private *dev_priv, const struct intel_sa_info *sa)
{
	struct intel_qgv_info qi = {};
	bool is_y_tile = true; /* assume y tile may be used */
	int num_channels;
	int deinterleave;
	int ipqdepth, ipqdepthpch;
	int dclk_max;
	int maxdebw;
	int i, ret;

	ret = icl_get_qgv_points(dev_priv, &qi);
	dev_priv->lispbsd_sagv.num_points = qi.num_points;
	dev_priv->lispbsd_sagv.num_channels = qi.num_channels;
	dev_priv->lispbsd_sagv.t_bl = qi.t_bl;
	dev_priv->lispbsd_sagv.dram_type = qi.dram_type;
	if (ret) {
		DRM_DEBUG_KMS("Failed to get memory subsystem information, ignoring bandwidth limits");
		/*
		 * LISPBSD: no QGV points to restrict or to check against;
		 * never send SAGV/QGV requests then.
		 */
		dev_priv->sagv_status = I915_SAGV_NOT_CONTROLLED;
		return ret;
	}
	num_channels = qi.num_channels;
	/* LISPBSD: the divisions below need channels (pre-existing hole) */
	if (num_channels == 0) {
		DRM_DEBUG_KMS("No memory channels reported, ignoring bandwidth limits\n");
		dev_priv->sagv_status = I915_SAGV_NOT_CONTROLLED;
		return -EINVAL;
	}

	deinterleave = DIV_ROUND_UP(num_channels, is_y_tile ? 4 : 2);
	dclk_max = icl_sagv_max_dclk(&qi);

	ipqdepthpch = 16;

	maxdebw = min(sa->deprogbwlimit * 1000,
		      icl_calc_bw(dclk_max, 16, 1) * 6 / 10); /* 60% */
	ipqdepth = min(ipqdepthpch, sa->displayrtids / num_channels);

	for (i = 0; i < ARRAY_SIZE(dev_priv->max_bw); i++) {
		struct intel_bw_info *bi = &dev_priv->max_bw[i];
		int clpchgroup;
		int j;

		clpchgroup = (sa->deburst * deinterleave / num_channels) << i;
		bi->num_planes = (ipqdepth - clpchgroup) / clpchgroup + 1;

		bi->num_qgv_points = qi.num_points;

		for (j = 0; j < qi.num_points; j++) {
			const struct intel_qgv_point *sp = &qi.points[j];
			int ct, bw;

			/*
			 * Max row cycle time
			 *
			 * FIXME what is the logic behind the
			 * assumed burst length?
			 */
			ct = max_t(int, sp->t_rc, sp->t_rp + sp->t_rcd +
				   (clpchgroup - 1) * qi.t_bl + sp->t_rdpre);
			bw = icl_calc_bw(sp->dclk, clpchgroup * 32 * num_channels, ct);

			bi->deratedbw[j] = min(maxdebw,
					       bw * 9 / 10); /* 90% */

			DRM_DEBUG_KMS("BW%d / QGV %d: num_planes=%d deratedbw=%u\n",
				      i, j, bi->num_planes, bi->deratedbw[j]);
		}

		if (bi->num_planes == 1)
			break;
	}

	/*
	 * In case if SAGV is disabled in BIOS, we always get 1
	 * SAGV point, but we can't send PCode commands to restrict it
	 * as it will fail and pointless anyway.
	 */
	if (qi.num_points < 2)	/* LISPBSD: also 0 points (upstream: == 1) */
		dev_priv->sagv_status = I915_SAGV_NOT_CONTROLLED;
	else
		dev_priv->sagv_status = I915_SAGV_ENABLED;

	return 0;
}

static unsigned int icl_max_bw(struct drm_i915_private *dev_priv,
			       int num_planes, int qgv_point)
{
	int i;

	/*
	 * Let's return max bw for 0 planes
	 */
	num_planes = max(1, num_planes);

	for (i = 0; i < ARRAY_SIZE(dev_priv->max_bw); i++) {
		const struct intel_bw_info *bi =
			&dev_priv->max_bw[i];

		/*
		 * Pcode will not expose all QGV points when
		 * SAGV is forced to off/min/med/max.
		 */
		if (qgv_point >= bi->num_qgv_points)
			return UINT_MAX;

		if (num_planes >= bi->num_planes)
			return bi->deratedbw[qgv_point];
	}

	return 0;
}

/*
 * LISPBSD: memory clock registers in the MCHBAR mirror.  MC_BIOS_REQ/DATA
 * are the SKL+ "BIOS request" pair (the low bits hold the requested QCLK
 * ratio); SA_PERF_STATUS is the current system agent operating point, laid
 * out as on DG1 (a TGL derivative): bits 9:2 QCLK ratio, bit 10 reference
 * (0 = 133.33 MHz, 1 = 100 MHz).  Only the raw values are certain on TGL.
 */
#define LISPBSD_MC_BIOS_REQ	_MMIO(MCHBAR_MIRROR_BASE_SNB + 0x5e00)
#define LISPBSD_MC_BIOS_DATA	_MMIO(MCHBAR_MIRROR_BASE_SNB + 0x5e04)
#define LISPBSD_SA_PERF_STATUS	_MMIO(MCHBAR_MIRROR_BASE_SNB + 0x5918)

/*
 * LISPBSD: the pcode gives a QGV point's DCLK in multiples of 16.667 MHz
 * (icl_calc_bw() works on the raw value); later upstream converts it with
 * DIV_ROUND_UP(16667 * dclk + (gen > 11 ? 500 : 0), 1000).
 */
static unsigned int lispbsd_dclk_mhz(struct drm_i915_private *dev_priv,
				     u32 raw)
{
	return DIV_ROUND_UP(16667 * (raw & 0xffff) +
			    (INTEL_GEN(dev_priv) > 11 ? 500 : 0), 1000);
}

static unsigned int lispbsd_qclk_mhz(u32 sa_perf)
{
	unsigned int ratio = (sa_perf >> 2) & 0xff;

	return (sa_perf & (1 << 10)) ? ratio * 100 : ratio * 13333 / 100;
}

static const char *lispbsd_sagv_status_str(int status)
{
	switch (status) {
	case I915_SAGV_UNKNOWN:		return "unknown";
	case I915_SAGV_DISABLED:	return "disabled";
	case I915_SAGV_ENABLED:		return "enabled";
	case I915_SAGV_NOT_CONTROLLED:	return "not-controlled";
	default:			return "?";
	}
}

static const char *lispbsd_dram_str(int type)
{
	switch (type) {
	case INTEL_DRAM_DDR3:	return "DDR3";
	case INTEL_DRAM_DDR4:	return "DDR4";
	case INTEL_DRAM_LPDDR3:	return "LPDDR3";
	case INTEL_DRAM_LPDDR4:	return "LPDDR4";
	default:		return "unknown";
	}
}

/*
 * LISPBSD: the text behind hw.i915sagv.  Everything but the live memory
 * clock registers is a snapshot taken at init or in the commit tail; @live
 * is false when the caller knows the device must not be touched.
 */
int intel_bw_lispbsd_sagv_report(struct drm_i915_private *dev_priv,
				 char *buf, size_t len, bool live)
{
	const struct intel_bw_info *bi = &dev_priv->max_bw[0];
	size_t off = 0;
	int i;

#define LISPBSD_P(...) do {						\
	if (off < len)							\
		off += snprintf(buf + off, len - off, __VA_ARGS__);	\
} while (0)

	LISPBSD_P("mode=%d applied=%u sagv_allowed=%d status=%s block_us=%d"
	    " block_ret=%d",
	    i915_modparams.lispbsd_sagv, dev_priv->lispbsd_sagv.mode,
	    dev_priv->lispbsd_sagv.sagv_allowed,
	    lispbsd_sagv_status_str(dev_priv->sagv_status),
	    (int)dev_priv->sagv_block_time_us,
	    dev_priv->lispbsd_sagv.block_time_ret);
	/* LISPBSD: the stock mode computes a mask but never sends it */
	if (dev_priv->lispbsd_sagv.mode == LISPBSD_SAGV_STOCK)
		LISPBSD_P(" mask=none");
	else
		LISPBSD_P(" mask=0x%x", dev_priv->lispbsd_sagv.qgv_points_mask);
	LISPBSD_P(" written=%d pinned=%d last=0x%x last_ret=%d writes=%u"
	    " errors=%u commits=%u",
	    dev_priv->lispbsd_sagv.mask_written,
	    dev_priv->lispbsd_sagv.pinned,
	    dev_priv->lispbsd_sagv.last_mask, dev_priv->lispbsd_sagv.last_ret,
	    dev_priv->lispbsd_sagv.writes, dev_priv->lispbsd_sagv.errors,
	    dev_priv->lispbsd_sagv.commits);
	LISPBSD_P(" global=0x%x/%d dram=%s ch=%u tbl=%u points=%u",
	    dev_priv->lispbsd_sagv.global_raw, dev_priv->lispbsd_sagv.global_ret,
	    lispbsd_dram_str(dev_priv->lispbsd_sagv.dram_type),
	    dev_priv->lispbsd_sagv.num_channels, dev_priv->lispbsd_sagv.t_bl,
	    dev_priv->lispbsd_sagv.num_points);
	for (i = 0; i < dev_priv->lispbsd_sagv.num_points &&
		 i < I915_NUM_QGV_POINTS; i++) {
		u32 val = dev_priv->lispbsd_sagv.qgv_raw[i][0];
		u32 val2 = dev_priv->lispbsd_sagv.qgv_raw[i][1];

		/* DCLK MHz, tRP tRCD tRDPRE tRAS, then MB/s for one plane */
		LISPBSD_P(" qgv%d=%u/%u/%u/%u/%u/%u", i,
		    lispbsd_dclk_mhz(dev_priv, val),
		    (val >> 16) & 0xff, (val >> 24) & 0xff,
		    val2 & 0xff, (val2 >> 8) & 0xff,
		    i < bi->num_qgv_points ? icl_max_bw(dev_priv, 1, i) : 0);
	}
	LISPBSD_P(" wm_us=");
	for (i = 0; i < ARRAY_SIZE(dev_priv->wm.skl_latency); i++)
		LISPBSD_P("%s%u", i ? "," : "", dev_priv->wm.skl_latency[i]);
	if (live) {
		u32 req = I915_READ(LISPBSD_MC_BIOS_REQ);
		u32 data = I915_READ(LISPBSD_MC_BIOS_DATA);
		u32 sa = I915_READ(LISPBSD_SA_PERF_STATUS);

		LISPBSD_P(" mc_req=0x%08x mc_data=0x%08x sa_perf=0x%08x"
		    " qclk_mhz=%u", req, data, sa, lispbsd_qclk_mhz(sa));
	} else {
		LISPBSD_P(" live=off");
	}
#undef LISPBSD_P

	return 0;
}

void intel_bw_init_hw(struct drm_i915_private *dev_priv)
{
	int i;

	if (!HAS_DISPLAY(dev_priv))
		return;

	if (IS_GEN(dev_priv, 12))
		icl_get_bw_info(dev_priv, &tgl_sa_info);
	else if (IS_GEN(dev_priv, 11))
		icl_get_bw_info(dev_priv, &icl_sa_info);
	else
		return;

	/*
	 * LISPBSD: say what the firmware allows before the driver touches
	 * anything (the SAGV switch is read at the first display updates).
	 */
	DRM_INFO("LISPBSD SAGV: pcode global info 0x%x (ret %d): %s, %u channels,"
	    " %u QGV points, sagv_status %s\n",
	    dev_priv->lispbsd_sagv.global_raw, dev_priv->lispbsd_sagv.global_ret,
	    lispbsd_dram_str(dev_priv->lispbsd_sagv.dram_type),
	    dev_priv->lispbsd_sagv.num_channels,
	    dev_priv->lispbsd_sagv.num_points,
	    lispbsd_sagv_status_str(dev_priv->sagv_status));
	for (i = 0; i < dev_priv->lispbsd_sagv.num_points &&
		 i < I915_NUM_QGV_POINTS; i++) {
		u32 val = dev_priv->lispbsd_sagv.qgv_raw[i][0];
		u32 val2 = dev_priv->lispbsd_sagv.qgv_raw[i][1];

		DRM_INFO("LISPBSD SAGV: QGV %d: DCLK=%u MHz (raw %u x 16.667)"
		    " tRP=%u tRCD=%u tRDPRE=%u tRAS=%u, %u MB/s for 1 plane"
		    " (ret %d)\n", i,
		    lispbsd_dclk_mhz(dev_priv, val), val & 0xffff,
		    (val >> 16) & 0xff, (val >> 24) & 0xff,
		    val2 & 0xff, (val2 >> 8) & 0xff,
		    i < dev_priv->max_bw[0].num_qgv_points ?
		    icl_max_bw(dev_priv, 1, i) : 0,
		    dev_priv->lispbsd_sagv.qgv_ret[i]);
	}
	{
		u32 req = I915_READ(LISPBSD_MC_BIOS_REQ);
		u32 data = I915_READ(LISPBSD_MC_BIOS_DATA);
		u32 sa = I915_READ(LISPBSD_SA_PERF_STATUS);

		DRM_INFO("LISPBSD SAGV: MC_BIOS_REQ 0x%08x MC_BIOS_DATA 0x%08x"
		    " SA_PERF_STATUS 0x%08x (QCLK %u MHz if DG1 layout)\n",
		    req, data, sa, lispbsd_qclk_mhz(sa));
	}
}

static unsigned int intel_bw_crtc_num_active_planes(const struct intel_crtc_state *crtc_state)
{
	/*
	 * We assume cursors are small enough
	 * to not not cause bandwidth problems.
	 */
	return hweight8(crtc_state->active_planes & ~BIT(PLANE_CURSOR));
}

static unsigned int intel_bw_crtc_data_rate(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	unsigned int data_rate = 0;
	enum plane_id plane_id;

	for_each_plane_id_on_crtc(crtc, plane_id) {
		/*
		 * We assume cursors are small enough
		 * to not not cause bandwidth problems.
		 */
		if (plane_id == PLANE_CURSOR)
			continue;

		data_rate += crtc_state->data_rate[plane_id];
	}

	return data_rate;
}

void intel_bw_crtc_update(struct intel_bw_state *bw_state,
			  const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	bw_state->data_rate[crtc->pipe] =
		intel_bw_crtc_data_rate(crtc_state);
	bw_state->num_active_planes[crtc->pipe] =
		intel_bw_crtc_num_active_planes(crtc_state);

	DRM_DEBUG_KMS("pipe %c data rate %u num active planes %u\n",
		      pipe_name(crtc->pipe),
		      bw_state->data_rate[crtc->pipe],
		      bw_state->num_active_planes[crtc->pipe]);
}

static unsigned int intel_bw_num_active_planes(struct drm_i915_private *dev_priv,
					       const struct intel_bw_state *bw_state)
{
	unsigned int num_active_planes = 0;
	enum pipe pipe;

	for_each_pipe(dev_priv, pipe)
		num_active_planes += bw_state->num_active_planes[pipe];

	return num_active_planes;
}

static unsigned int intel_bw_data_rate(struct drm_i915_private *dev_priv,
				       const struct intel_bw_state *bw_state)
{
	unsigned int data_rate = 0;
	enum pipe pipe;

	for_each_pipe(dev_priv, pipe)
		data_rate += bw_state->data_rate[pipe];

	return data_rate;
}

struct intel_bw_state *
intel_atomic_get_old_bw_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *bw_state;

	bw_state = intel_atomic_get_old_global_obj_state(state, &dev_priv->bw_obj);

	return to_intel_bw_state(bw_state);
}

struct intel_bw_state *
intel_atomic_get_new_bw_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *bw_state;

	bw_state = intel_atomic_get_new_global_obj_state(state, &dev_priv->bw_obj);

	return to_intel_bw_state(bw_state);
}

struct intel_bw_state *
intel_atomic_get_bw_state(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_global_state *bw_state;

	bw_state = intel_atomic_get_global_obj_state(state, &dev_priv->bw_obj);
	if (IS_ERR(bw_state))
		return ERR_CAST(bw_state);

	return to_intel_bw_state(bw_state);
}

int intel_bw_atomic_check(struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *new_crtc_state, *old_crtc_state;
	struct intel_bw_state *new_bw_state = NULL;
	const struct intel_bw_state *old_bw_state = NULL;
	unsigned int data_rate;
	unsigned int num_active_planes;
	struct intel_crtc *crtc;
	int i, ret;
	u32 allowed_points = 0;
	unsigned int max_bw_point = 0, max_bw = 0;
	unsigned int num_qgv_points = dev_priv->max_bw[0].num_qgv_points;
	u32 mask = (1 << num_qgv_points) - 1;
	bool changed = false;

	/* FIXME earlier gens need some checks too */
	if (INTEL_GEN(dev_priv) < 11)
		return 0;

	/*
	 * LISPBSD: without QGV information from the pcode (the read failed
	 * at init) there is nothing to check against or to restrict; the
	 * code before the SAGV series did not limit the bandwidth then
	 * either, where this would reject every configuration.
	 */
	if (num_qgv_points == 0)
		return 0;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		unsigned int old_data_rate =
			intel_bw_crtc_data_rate(old_crtc_state);
		unsigned int new_data_rate =
			intel_bw_crtc_data_rate(new_crtc_state);
		unsigned int old_active_planes =
			intel_bw_crtc_num_active_planes(old_crtc_state);
		unsigned int new_active_planes =
			intel_bw_crtc_num_active_planes(new_crtc_state);

		/*
		 * Avoid locking the bw state when
		 * nothing significant has changed.
		 */
		if (old_data_rate == new_data_rate &&
		    old_active_planes == new_active_planes)
			continue;

		new_bw_state = intel_atomic_get_bw_state(state);
		if (IS_ERR(new_bw_state))
			return PTR_ERR(new_bw_state);

		new_bw_state->data_rate[crtc->pipe] = new_data_rate;
		new_bw_state->num_active_planes[crtc->pipe] = new_active_planes;

		changed = true;

		DRM_DEBUG_KMS("pipe %c data rate %u num active planes %u\n",
			      pipe_name(crtc->pipe),
			      new_bw_state->data_rate[crtc->pipe],
			      new_bw_state->num_active_planes[crtc->pipe]);
	}

	old_bw_state = intel_atomic_get_old_bw_state(state);
	new_bw_state = intel_atomic_get_new_bw_state(state);

	if (new_bw_state &&
	    (intel_can_enable_sagv(dev_priv, old_bw_state) !=
	     intel_can_enable_sagv(dev_priv, new_bw_state) ||
	     /* LISPBSD: the SAGV switch changed */
	     old_bw_state->lispbsd_sagv != new_bw_state->lispbsd_sagv))
		changed = true;

	/*
	 * If none of our inputs (data rates, number of active
	 * planes, SAGV yes/no) changed then nothing to do here.
	 */
	if (!changed)
		return 0;

	ret = intel_atomic_lock_global_state(&new_bw_state->base);
	if (ret)
		return ret;

	data_rate = intel_bw_data_rate(dev_priv, new_bw_state);
	data_rate = DIV_ROUND_UP(data_rate, 1000);

	num_active_planes = intel_bw_num_active_planes(dev_priv, new_bw_state);

	for (i = 0; i < num_qgv_points; i++) {
		unsigned int max_data_rate;

		max_data_rate = icl_max_bw(dev_priv, num_active_planes, i);
		/*
		 * We need to know which qgv point gives us
		 * maximum bandwidth in order to disable SAGV
		 * if we find that we exceed SAGV block time
		 * with watermarks. By that moment we already
		 * have those, as it is calculated earlier in
		 * intel_atomic_check,
		 */
		if (max_data_rate > max_bw) {
			max_bw_point = i;
			max_bw = max_data_rate;
		}
		if (max_data_rate >= data_rate)
			allowed_points |= BIT(i);
		drm_dbg_kms(&dev_priv->drm, "QGV point %d: max bw %d required %d\n",
			    i, max_data_rate, data_rate);
	}

	/*
	 * BSpec states that we always should have at least one allowed point
	 * left, so if we couldn't - simply reject the configuration for obvious
	 * reasons.
	 */
	if (allowed_points == 0) {
		drm_dbg_kms(&dev_priv->drm, "No QGV points provide sufficient memory"
			    " bandwidth %d for display configuration(%d active planes).\n",
			    data_rate, num_active_planes);
		return -EINVAL;
	}

	/*
	 * LISPBSD: in the stock mode the driver restricts no QGV point and,
	 * unless a pin to the highest bandwidth point succeeded, the
	 * firmware may run the memory at any of them, so every point has
	 * to carry the configuration (the check before the SAGV series).
	 * After a successful pin (mode 1 or 2 was used and left for stock)
	 * the upstream rule below is the right one; the stock rule would
	 * make every commit fail on a configuration only that point carries.
	 * A failed pin leaves the firmware in charge: keep the stock rule.
	 */
	if (INTEL_GEN(dev_priv) >= 12 &&
	    new_bw_state->lispbsd_sagv == LISPBSD_SAGV_STOCK &&
	    !READ_ONCE(dev_priv->lispbsd_sagv.pinned) &&
	    allowed_points != mask) {
		drm_dbg_kms(&dev_priv->drm, "Bandwidth %u MB/s exceeds max available"
			    " on QGV points 0x%x (%d active planes)\n",
			    data_rate, ~allowed_points & mask, num_active_planes);
		return -EINVAL;
	}

	/* LISPBSD: what "SAGV off" means for this configuration */
	new_bw_state->lispbsd_sagv_off_mask = ~BIT(max_bw_point) & mask;

	/*
	 * Leave only single point with highest bandwidth, if
	 * we can't enable SAGV due to the increased memory latency it may
	 * cause.
	 */
	if (!intel_can_enable_sagv(dev_priv, new_bw_state)) {
		allowed_points = BIT(max_bw_point);
		drm_dbg_kms(&dev_priv->drm, "No SAGV, using single QGV point %d\n",
			    max_bw_point);
	}
	/*
	 * We store the ones which need to be masked as that is what PCode
	 * actually accepts as a parameter.
	 */
	new_bw_state->qgv_points_mask = ~allowed_points & mask;

	/*
	 * If the actual mask had changed we need to make sure that
	 * the commits are serialized(in case this is a nomodeset, nonblocking)
	 */
	if (new_bw_state->qgv_points_mask != old_bw_state->qgv_points_mask) {
		ret = intel_atomic_serialize_global_state(&new_bw_state->base);
		if (ret)
			return ret;
	}

	return 0;
}

static struct intel_global_state *
intel_bw_duplicate_state(struct intel_global_obj *obj)
{
	struct intel_bw_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	return &state->base;
}

static void intel_bw_destroy_state(struct intel_global_obj *obj,
				   struct intel_global_state *state)
{
	kfree(state);
}

static const struct intel_global_state_funcs intel_bw_funcs = {
	.atomic_duplicate_state = intel_bw_duplicate_state,
	.atomic_destroy_state = intel_bw_destroy_state,
};

int intel_bw_init(struct drm_i915_private *dev_priv)
{
	struct intel_bw_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	intel_atomic_global_obj_init(dev_priv, &dev_priv->bw_obj,
				     &state->base, &intel_bw_funcs);

	return 0;
}
