/*	$NetBSD: i915_pci_autoconf.c,v 1.14 2022/10/15 15:20:06 riastradh Exp $	*/

/*-
 * Copyright (c) 2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Taylor R. Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: i915_pci_autoconf.c,v 1.14 2022/10/15 15:20:06 riastradh Exp $");

#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/workqueue.h>
#include <sys/sysctl.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <drm/drm_ioctl.h>
#include <drm/drm_pci.h>

#include "i915_drv.h"
#include "i915_pci.h"
#include "gt/intel_rps.h"

struct drm_device;

SIMPLEQ_HEAD(i915drmkms_task_head, i915drmkms_task);

struct i915drmkms_softc {
	device_t			sc_dev;
	struct pci_attach_args		sc_pa;
	struct lwp			*sc_task_thread;
	struct i915drmkms_task_head	sc_tasks;
	struct workqueue		*sc_task_wq;
	struct drm_device		*sc_drm_dev;
	struct pci_dev			sc_pci_dev;
};

static const struct pci_device_id *
		i915drmkms_pci_lookup(const struct pci_attach_args *);

static int	i915drmkms_match(device_t, cfdata_t, void *);
static void	i915drmkms_attach(device_t, device_t, void *);
static void	i915drmkms_attach_real(device_t);
static int	i915drmkms_detach(device_t, int);

static bool	i915drmkms_suspend(device_t, const pmf_qual_t *);
static bool	i915drmkms_resume(device_t, const pmf_qual_t *);

static void	i915drmkms_task_work(struct work *, void *);

/* LISPBSD: transparent i915 runtime PM, triggered via hw.i915rpm sysctl. */
int i915_lispbsd_rpm_suspend(struct drm_device *);
int i915_lispbsd_rpm_resume(struct drm_device *);
extern int i915_lispbsd_s0idle;

static struct i915drmkms_softc *lispbsd_i915_sc;
static int lispbsd_i915rpm;	/* 0 active, 1 rpm-suspended, 2 rpm+D3 */

static int
i915drmkms_sysctl_rpm(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct i915drmkms_softc *sc = lispbsd_i915_sc;
	int val = lispbsd_i915rpm;
	int error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (val < 0 || val > 2)
		return EINVAL;
	if (sc == NULL || sc->sc_drm_dev == NULL)
		return ENXIO;
	if (val == lispbsd_i915rpm)
		return 0;

	/* leaving D3 -> restore D0 first */
	if (lispbsd_i915rpm == 2 && val < 2)
		pci_set_powerstate(sc->sc_pa.pa_pc, sc->sc_pa.pa_tag,
		    PCI_PMCSR_STATE_D0);
	/* entering runtime suspend from active */
	if (lispbsd_i915rpm == 0 && val >= 1) {
		error = i915_lispbsd_rpm_suspend(sc->sc_drm_dev);
		if (error)
			return -error;
	}
	/* entering D3 */
	if (val == 2 && lispbsd_i915rpm < 2)
		pci_set_powerstate(sc->sc_pa.pa_pc, sc->sc_pa.pa_tag,
		    PCI_PMCSR_STATE_D3);
	/* returning to active */
	if (val == 0 && lispbsd_i915rpm >= 1) {
		error = i915_lispbsd_rpm_resume(sc->sc_drm_dev);
		if (error)
			return -error;
	}
	lispbsd_i915rpm = val;
	return 0;
}

/* Software-only sample: reading this must not acquire a GPU wakeref. */
static int
i915drmkms_sysctl_pmstate(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct i915drmkms_softc *sc = lispbsd_i915_sc;
	struct intel_gt *gt;
	struct intel_uncore *uncore;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	char buf[768];
	size_t off;
	unsigned i;

	if (sc == NULL || sc->sc_drm_dev == NULL)
		return ENXIO;
	gt = &to_i915(sc->sc_drm_dev)->gt;
	uncore = gt->uncore;
	off = snprintf(buf, sizeof(buf),
	    "gt=%d user=%d awake=%d rc6=%d fw_active=0x%x fw_timer=0x%x fw_user=%u",
	    atomic_read(&gt->wakeref.count), atomic_read(&gt->user_wakeref),
	    READ_ONCE(gt->awake) != 0, gt->rc6.enabled,
	    READ_ONCE(uncore->fw_domains_active),
	    READ_ONCE(uncore->fw_domains_timer),
	    READ_ONCE(uncore->user_forcewake_count));
	for (i = 0; i < FW_DOMAIN_ID_COUNT && off < sizeof(buf); i++) {
		if (uncore->fw_domain[i] == NULL)
			continue;
		off += snprintf(buf + off, sizeof(buf) - off, " fw%u=%u", i,
		    READ_ONCE(uncore->fw_domain[i]->wake_count));
	}
	for_each_engine(engine, gt, id) {
		if (off >= sizeof(buf))
			break;
		off += snprintf(buf + off, sizeof(buf) - off, " %s=%d",
		    engine->name, atomic_read(&engine->wakeref.count));
	}
	node.sysctl_data = buf;
	node.sysctl_size = strlen(buf) + 1;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

/*
 * LISPBSD: GPU frequency scaling (RPS) state and limits, hw.i915rps.*.
 * Frequencies are MHz.  max_mhz also caps boost_mhz, so it is a hard cap
 * (rps_work ignores the soft limit for a wait-boost); min_mhz and boost_mhz
 * are the usual soft limits.  Reading act_mhz does not wake the GPU: it is
 * 0 while the GT is parked.
 */
enum {
	RPS_ACT, RPS_CUR, RPS_RP0, RPS_RP1, RPS_RPE, RPS_RPN,
	RPS_MAX, RPS_MIN, RPS_BOOST, RPS_IDLE, RPS_NSEL
};
static int lispbsd_rps_sel[RPS_NSEL] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

static int
i915drmkms_sysctl_rps_mhz(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct i915drmkms_softc *sc = lispbsd_i915_sc;
	struct intel_rps *rps;
	int sel = *(const int *)rnode->sysctl_data;
	int val, error, opcode;

	if (sc == NULL || sc->sc_drm_dev == NULL)
		return ENXIO;
	rps = &to_i915(sc->sc_drm_dev)->gt.rps;
	switch (sel) {
	case RPS_ACT:	val = intel_rps_read_actual_frequency(rps); break;
	case RPS_CUR:	val = intel_gpu_freq(rps, READ_ONCE(rps->cur_freq)); break;
	case RPS_RP0:	val = intel_gpu_freq(rps, rps->rp0_freq); break;
	case RPS_RP1:	val = intel_gpu_freq(rps, rps->rp1_freq); break;
	case RPS_RPE:	val = intel_gpu_freq(rps, rps->efficient_freq); break;
	case RPS_RPN:	val = intel_gpu_freq(rps, rps->min_freq); break;
	case RPS_MAX:	val = intel_gpu_freq(rps, rps->max_freq_softlimit); break;
	case RPS_MIN:	val = intel_gpu_freq(rps, rps->min_freq_softlimit); break;
	case RPS_BOOST:	val = intel_gpu_freq(rps, rps->boost_freq); break;
	case RPS_IDLE:	val = intel_gpu_freq(rps, rps->idle_freq); break;
	default:	return EINVAL;
	}
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (sel != RPS_MAX && sel != RPS_MIN && sel != RPS_BOOST)
		return EPERM;
	if (!rps->enabled)
		return ENXIO;

	opcode = intel_freq_opcode(rps, val);
	if (opcode < rps->min_freq || opcode > rps->max_freq)
		return EINVAL;
	mutex_lock(&rps->lock);
	switch (sel) {
	case RPS_MAX:
		if (opcode < rps->min_freq_softlimit) {
			error = EINVAL;
			break;
		}
		rps->max_freq_softlimit = opcode;
		if (rps->boost_freq > opcode)
			rps->boost_freq = opcode;
		break;
	case RPS_MIN:
		if (opcode > rps->max_freq_softlimit) {
			error = EINVAL;
			break;
		}
		rps->min_freq_softlimit = opcode;
		break;
	case RPS_BOOST:
		rps->boost_freq = opcode;
		break;
	}
	if (error == 0 && sel != RPS_BOOST) {
		/* Re-clamp the request and the interrupt limits, as sysfs does. */
		val = clamp_t(int, rps->cur_freq, rps->min_freq_softlimit,
		    rps->max_freq_softlimit);
		if (intel_rps_set(rps, val))
			error = EIO;
	}
	mutex_unlock(&rps->lock);
	return error;
}

static int
i915drmkms_sysctl_rps_stats(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct i915drmkms_softc *sc = lispbsd_i915_sc;
	struct intel_rps *rps;
	char buf[320];

	if (sc == NULL || sc->sc_drm_dev == NULL)
		return ENXIO;
	rps = &to_i915(sc->sc_drm_dev)->gt.rps;
	snprintf(buf, sizeof(buf),
	    "irq_raw=%llu irq=%llu boost=%llu up=%llu timeout=%llu down=%llu"
	    " unknown=%llu park=%llu unpark=%llu set=%llu"
	    " enabled=%d active=%d pm_events=0x%x power=%d"
	    " last_adj=%d waiters=%d boosts=%d",
	    (unsigned long long)lispbsd_rps_stats.irq_raw,
	    (unsigned long long)lispbsd_rps_stats.irq,
	    (unsigned long long)lispbsd_rps_stats.boost,
	    (unsigned long long)lispbsd_rps_stats.up,
	    (unsigned long long)lispbsd_rps_stats.timeout,
	    (unsigned long long)lispbsd_rps_stats.down,
	    (unsigned long long)lispbsd_rps_stats.unknown,
	    (unsigned long long)lispbsd_rps_stats.park,
	    (unsigned long long)lispbsd_rps_stats.unpark,
	    (unsigned long long)lispbsd_rps_stats.set,
	    rps->enabled, rps->active, READ_ONCE(rps->pm_events),
	    rps->power.mode, rps->last_adj, atomic_read(&rps->num_waiters),
	    atomic_read(&rps->boosts));
	node.sysctl_data = buf;
	node.sysctl_size = strlen(buf) + 1;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

static void
i915drmkms_sysctl_rps_init(void)
{
	static const char *const names[RPS_NSEL] = {
		"act_mhz", "cur_mhz", "rp0_mhz", "rp1_mhz", "rpe_mhz", "rpn_mhz",
		"max_mhz", "min_mhz", "boost_mhz", "idle_mhz"
	};
	static const char *const descs[RPS_NSEL] = {
		"actual GT frequency (0 while parked)",
		"frequency the driver currently requests when awake",
		"RP0, hardware maximum", "RP1, guaranteed", "RPe, efficient",
		"RPn, hardware minimum",
		"software maximum, also caps boost_mhz (hard cap)",
		"software minimum", "frequency for a client wait-boost",
		"frequency requested when parked"
	};
	const struct sysctlnode *rnode = NULL;
	int i;

	/* Not CTLFLAG_PERMANENT: the drm attach runs after the sysctl root
	 * is sealed, and sysctl_create refuses permanent nodes by then. */
	if (sysctl_createv(NULL, 0, NULL, &rnode,
	    0, CTLTYPE_NODE, "i915rps",
	    SYSCTL_DESCR("LISPBSD GPU frequency scaling (RPS) state and limits"),
	    NULL, 0, NULL, 0, CTL_HW, CTL_CREATE, CTL_EOL) != 0)
		return;
	for (i = 0; i < RPS_NSEL; i++)
		(void)sysctl_createv(NULL, 0, &rnode, NULL,
		    (i == RPS_MAX || i == RPS_MIN || i == RPS_BOOST) ?
		    CTLFLAG_READWRITE : CTLFLAG_READONLY,
		    CTLTYPE_INT, names[i], SYSCTL_DESCR(descs[i]),
		    i915drmkms_sysctl_rps_mhz, 0, &lispbsd_rps_sel[i], 0,
		    CTL_CREATE, CTL_EOL);
	(void)sysctl_createv(NULL, 0, &rnode, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "unpark_start",
	    SYSCTL_DESCR("frequency at unpark: 0 max(last, RPe) [stock], 1 RPe, 2 RPn"),
	    NULL, 0, &lispbsd_rps_unpark_start, 0, CTL_CREATE, CTL_EOL);
	(void)sysctl_createv(NULL, 0, &rnode, NULL,
	    CTLFLAG_READONLY, CTLTYPE_STRING, "stats",
	    SYSCTL_DESCR("RPS event counters and state"),
	    i915drmkms_sysctl_rps_stats, 0, NULL, 0, CTL_CREATE, CTL_EOL);
}

CFATTACH_DECL_NEW(i915drmkms, sizeof(struct i915drmkms_softc),
    i915drmkms_match, i915drmkms_attach, i915drmkms_detach, NULL);

/* XXX Kludge to get these from i915_pci.c.  */
extern const struct pci_device_id *const i915_device_ids;
extern const size_t i915_n_device_ids;

static const struct pci_device_id *
i915drmkms_pci_lookup(const struct pci_attach_args *pa)
{
	size_t i;

	/* Attach only at function 0 to work around Intel lossage.  */
	if (pa->pa_function != 0)
		return NULL;

	/* We're interested only in Intel products.  */
	if (PCI_VENDOR(pa->pa_id) != PCI_VENDOR_INTEL)
		return NULL;

	/* We're interested only in Intel display devices.  */
	if (PCI_CLASS(pa->pa_class) != PCI_CLASS_DISPLAY)
		return NULL;

	for (i = 0; i < i915_n_device_ids; i++)
		if (PCI_PRODUCT(pa->pa_id) == i915_device_ids[i].device)
			break;

	/* Did we find it?  */
	if (i == i915_n_device_ids)
		return NULL;

	const struct pci_device_id *ent = &i915_device_ids[i];
	const struct intel_device_info *const info =
	    (struct intel_device_info *)ent->driver_data;

	if (info->require_force_probe) {
		/*
		 * LispBSD: force-probe preliminary hardware (Tiger Lake and
		 * friends) instead of refusing to attach.
		 */
		printf("i915drmkms: preliminary hardware support forced on\n");
	}

	return ent;
}

static int
i915drmkms_match(device_t parent, cfdata_t match, void *aux)
{
	extern int i915drmkms_guarantee_initialized(void);
	const struct pci_attach_args *const pa = aux;
	int error;

	error = i915drmkms_guarantee_initialized();
	if (error) {
		aprint_error("i915drmkms: failed to initialize: %d\n", error);
		return 0;
	}

	if (i915drmkms_pci_lookup(pa) == NULL)
		return 0;

	return 6;		/* XXX Beat genfb_pci...  */
}

static void
i915drmkms_attach(device_t parent, device_t self, void *aux)
{
	struct i915drmkms_softc *const sc = device_private(self);
	const struct pci_attach_args *const pa = aux;
	int error;

	pci_aprint_devinfo(pa, NULL);

	/* Initialize the Linux PCI device descriptor.  */
	linux_pci_dev_init(&sc->sc_pci_dev, self, parent, pa, 0);

	sc->sc_dev = self;
	sc->sc_pa = *pa;
	sc->sc_task_thread = NULL;
	SIMPLEQ_INIT(&sc->sc_tasks);
	error = workqueue_create(&sc->sc_task_wq, "intelfb",
	    &i915drmkms_task_work, NULL, PRI_NONE, IPL_NONE, WQ_MPSAFE);
	if (error) {
		aprint_error_dev(self, "unable to create workqueue: %d\n",
		    error);
		sc->sc_task_wq = NULL;
		return;
	}

	/*
	 * Defer the remainder of initialization until we have mounted
	 * the root file system and can load firmware images.
	 */
	config_mountroot(self, &i915drmkms_attach_real);
}

static void
i915drmkms_attach_real(device_t self)
{
	struct i915drmkms_softc *const sc = device_private(self);
	struct pci_attach_args *const pa = &sc->sc_pa;
	const struct pci_device_id *ent = i915drmkms_pci_lookup(pa);
	const struct intel_device_info *const info __diagused =
	    (struct intel_device_info *)ent->driver_data;
	int error;

	KASSERT(info != NULL);

	/*
	 * Cause any tasks issued synchronously during attach to be
	 * processed at the end of this function.
	 */
	sc->sc_task_thread = curlwp;

	/* Attach the drm driver.  */
	/* XXX errno Linux->NetBSD */
	error = -i915_driver_probe(&sc->sc_pci_dev, ent);
	if (error) {
		aprint_error_dev(self, "unable to register drm: %d\n", error);
		return;
	}
	sc->sc_drm_dev = pci_get_drvdata(&sc->sc_pci_dev);

	/* LISPBSD: expose runtime-PM trigger and remember our softc. */
	lispbsd_i915_sc = sc;
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "i915rpm",
	    SYSCTL_DESCR("LISPBSD i915 runtime PM (0=on 1=disp-off 2=+D3)"),
	    i915drmkms_sysctl_rpm, 0, NULL, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);

	(void)sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_READWRITE, CTLTYPE_INT, "i915s0idle",
	    SYSCTL_DESCR("LISPBSD force i915 S0-idle suspend (preserve RC6)"),
	    NULL, 0, &i915_lispbsd_s0idle, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);

	(void)sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_READONLY, CTLTYPE_STRING, "i915pmstate",
	    SYSCTL_DESCR("i915 software wakeref snapshot without waking the GPU"),
	    i915drmkms_sysctl_pmstate, 0, NULL, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);
	i915drmkms_sysctl_rps_init();

	/*
	 * Now that the drm driver is attached, we can safely suspend
	 * and resume.
	 */
	if (!pmf_device_register(self, &i915drmkms_suspend,
		&i915drmkms_resume))
		aprint_error_dev(self, "unable to establish power handler\n");

	/*
	 * Process asynchronous tasks queued synchronously during
	 * attach.  This will be for display detection to attach a
	 * framebuffer, so we have the opportunity for a console device
	 * to attach before autoconf has completed, in time for init(8)
	 * to find that console without panicking.
	 */
	while (!SIMPLEQ_EMPTY(&sc->sc_tasks)) {
		struct i915drmkms_task *const task =
		    SIMPLEQ_FIRST(&sc->sc_tasks);

		SIMPLEQ_REMOVE_HEAD(&sc->sc_tasks, ift_u.queue);
		(*task->ift_fn)(task);
	}

	/* Cause any subesquent tasks to be processed by the workqueue.  */
	atomic_store_relaxed(&sc->sc_task_thread, NULL);
}

static int
i915drmkms_detach(device_t self, int flags)
{
	struct i915drmkms_softc *const sc = device_private(self);
	int error;

	/* XXX Check for in-use before tearing it all down...  */
	error = config_detach_children(self, flags);
	if (error)
		return error;

	KASSERT(sc->sc_task_thread == NULL);
	KASSERT(SIMPLEQ_EMPTY(&sc->sc_tasks));

	pmf_device_deregister(self);
	if (sc->sc_drm_dev) {
		i915_driver_remove(sc->sc_drm_dev->dev_private);
		sc->sc_drm_dev = NULL;
	}
	if (sc->sc_task_wq) {
		workqueue_destroy(sc->sc_task_wq);
		sc->sc_task_wq = NULL;
	}
	linux_pci_dev_destroy(&sc->sc_pci_dev);

	return 0;
}

static bool
i915drmkms_suspend(device_t self, const pmf_qual_t *qual)
{
	struct i915drmkms_softc *const sc = device_private(self);
	struct drm_device *const dev = sc->sc_drm_dev;
	int ret;

	drm_suspend_ioctl(dev);

	ret = i915_drm_prepare(dev);
	if (ret)
		return false;
	ret = i915_drm_suspend(dev);
	if (ret)
		return false;
	ret = i915_drm_suspend_late(dev, false);
	if (ret)
		return false;

	/* LISPBSD: force the GPU to D3 so the CPU package can reach PC8+.
	 * i915_drm_suspend_late skips this on NetBSD ("pmf handles this")
	 * but pmf leaves it in D0, pinning the package out of deep C-states. */
	pci_set_powerstate(sc->sc_pa.pa_pc, sc->sc_pa.pa_tag,
	    PCI_PMCSR_STATE_D3);

	return true;
}

static bool
i915drmkms_resume(device_t self, const pmf_qual_t *qual)
{
	struct i915drmkms_softc *const sc = device_private(self);
	struct drm_device *const dev = sc->sc_drm_dev;
	int ret;

	/* LISPBSD: restore D0 before touching GPU registers (forced D3). */
	pci_set_powerstate(sc->sc_pa.pa_pc, sc->sc_pa.pa_tag,
	    PCI_PMCSR_STATE_D0);

	ret = i915_drm_resume_early(dev);
	if (ret)
		goto out;
	ret = i915_drm_resume(dev);
	if (ret)
		goto out;

out:	drm_resume_ioctl(dev);
	return ret == 0;
}

static void
i915drmkms_task_work(struct work *work, void *cookie __unused)
{
	struct i915drmkms_task *const task = container_of(work,
	    struct i915drmkms_task, ift_u.work);

	(*task->ift_fn)(task);
}

void
i915drmkms_task_schedule(device_t self, struct i915drmkms_task *task)
{
	struct i915drmkms_softc *const sc = device_private(self);

	if (atomic_load_relaxed(&sc->sc_task_thread) == curlwp)
		SIMPLEQ_INSERT_TAIL(&sc->sc_tasks, task, ift_u.queue);
	else
		workqueue_enqueue(sc->sc_task_wq, &task->ift_u.work, NULL);
}
