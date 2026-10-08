/*	$NetBSD: intel_rps.h,v 1.4 2021/12/19 12:32:15 riastradh Exp $	*/

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_RPS_H
#define INTEL_RPS_H

#include "intel_rps_types.h"

struct i915_request;

void intel_rps_init_early(struct intel_rps *rps);
void intel_rps_init(struct intel_rps *rps);
void intel_rps_fini(struct intel_rps *rps);

void intel_rps_driver_register(struct intel_rps *rps);
void intel_rps_driver_unregister(struct intel_rps *rps);

void intel_rps_enable(struct intel_rps *rps);
void intel_rps_disable(struct intel_rps *rps);

void intel_rps_park(struct intel_rps *rps);
void intel_rps_unpark(struct intel_rps *rps);
void intel_rps_boost(struct i915_request *rq);

int intel_rps_set(struct intel_rps *rps, u8 val);
void intel_rps_mark_interactive(struct intel_rps *rps, bool interactive);

int intel_gpu_freq(struct intel_rps *rps, int val);
int intel_freq_opcode(struct intel_rps *rps, int val);
u32 intel_rps_get_cagf(struct intel_rps *rps, u32 rpstat1);
u32 intel_rps_read_actual_frequency(struct intel_rps *rps);

void gen5_rps_irq_handler(struct intel_rps *rps);
void gen6_rps_irq_handler(struct intel_rps *rps, u32 pm_iir);
void gen11_rps_irq_handler(struct intel_rps *rps, u32 pm_iir);

extern spinlock_t mchdev_lock;

#ifdef __NetBSD__
/*
 * LISPBSD: event counters for the RPS path and the unpark start policy,
 * exposed through hw.i915rps (i915_pci_autoconf.c).  The GT parks between
 * frames on a desktop, so the interrupt-driven scaling rarely gets an
 * evaluation interval and the frequency it restores on unpark is what the
 * GPU actually runs at.
 */
struct lispbsd_rps_stats {
	u64 irq_raw;	/* gen11_rps_irq_handler calls */
	u64 irq;	/* ... with an event we listen for (work scheduled) */
	u64 boost;	/* rps_work: client wait-boost to boost_freq */
	u64 up;		/* rps_work: UP_THRESHOLD */
	u64 timeout;	/* rps_work: DOWN_TIMEOUT (idle for a second) */
	u64 down;	/* rps_work: DOWN_THRESHOLD */
	u64 unknown;	/* rps_work: no event bit we understand */
	u64 park;
	u64 unpark;
	u64 set;	/* intel_rps_set calls */
};
extern struct lispbsd_rps_stats lispbsd_rps_stats;
/* 0: stock, max(cur_freq, RPe); 1: start every unpark at RPe; 2: at RPn */
extern int lispbsd_rps_unpark_start;
#define LISPBSD_RPS_COUNT(f)	((void)lispbsd_rps_stats.f++)
#else
#define LISPBSD_RPS_COUNT(f)	((void)0)
#endif

#endif /* INTEL_RPS_H */
