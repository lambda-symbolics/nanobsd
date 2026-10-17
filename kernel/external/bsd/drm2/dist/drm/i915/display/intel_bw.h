/*	$NetBSD: intel_bw.h,v 1.2 2021/12/18 23:45:29 riastradh Exp $	*/

/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_BW_H__
#define __INTEL_BW_H__

#include <drm/drm_atomic.h>

#include "intel_display.h"
#include "intel_global_state.h"

struct drm_i915_private;
struct intel_atomic_state;
struct intel_crtc_state;

struct intel_bw_state {
	struct intel_global_state base;

	/*
	 * Contains a bit mask, used to determine, whether correspondent
	 * pipe allows SAGV or not.
	 */
	u8 pipe_sagv_reject;

	/*
	 * Current QGV points mask, which restricts
	 * some particular SAGV states, not to confuse
	 * with pipe_sagv_mask.
	 */
	u8 qgv_points_mask;

	unsigned int data_rate[I915_MAX_PIPES];
	u8 num_active_planes[I915_MAX_PIPES];

	/* bitmask of active pipes */
	u8 active_pipes;

	/*
	 * LISPBSD: hw.drm2.i915_modparams.lispbsd_sagv as sampled by
	 * intel_compute_sagv_mask() for this state (gen12 only), and the
	 * QGV mask that leaves only the highest bandwidth point.
	 */
	u8 lispbsd_sagv;
	u8 lispbsd_sagv_off_mask;
};

/* LISPBSD: values of lispbsd_sagv */
#define LISPBSD_SAGV_STOCK	0	/* never touch SAGV/QGV, plain wm0 */
#define LISPBSD_SAGV_AUTO	1	/* upstream TGL SAGV handling */
#define LISPBSD_SAGV_OFF	2	/* managed, highest bandwidth point only */

#define to_intel_bw_state(x) container_of((x), struct intel_bw_state, base)

struct intel_bw_state *
intel_atomic_get_old_bw_state(struct intel_atomic_state *state);

struct intel_bw_state *
intel_atomic_get_new_bw_state(struct intel_atomic_state *state);

struct intel_bw_state *
intel_atomic_get_bw_state(struct intel_atomic_state *state);

void intel_bw_init_hw(struct drm_i915_private *dev_priv);
int intel_bw_init(struct drm_i915_private *dev_priv);
int intel_bw_atomic_check(struct intel_atomic_state *state);
void intel_bw_crtc_update(struct intel_bw_state *bw_state,
			  const struct intel_crtc_state *crtc_state);
int icl_pcode_restrict_qgv_points(struct drm_i915_private *dev_priv,
				  u32 points_mask);
int intel_bw_lispbsd_sagv_report(struct drm_i915_private *dev_priv,
				 char *buf, size_t len, bool live);

#endif /* __INTEL_BW_H__ */
