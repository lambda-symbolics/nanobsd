/* $NetBSD: acpi_power.c,v 1.36 2022/05/31 20:28:57 mrg Exp $ */

/*-
 * Copyright (c) 2009, 2010, 2011 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jukka Ruohonen.
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

/*-
 * Copyright (c) 2001 Michael Smith
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* The host harness includes the production backend with kernel adapters. */
#ifndef ACPI_POWER_TEST
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: acpi_power.c,v 1.36 2022/05/31 20:28:57 mrg Exp $");
#include "pci.h"
#include <sys/param.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/once.h>
#include <sys/sysctl.h>
#include <dev/acpi/acpireg.h>
#include <dev/acpi/acpivar.h>
#include <dev/acpi/acpi_pci.h>
#include <dev/acpi/acpi_power.h>
#endif

#define _COMPONENT ACPI_BUS_COMPONENT
ACPI_MODULE_NAME("acpi_power")

/* One consumer may own both a device-state and a _PRW/driver reference. */
#define POWER_STATE 1U
#define POWER_WAKE  2U

struct power_ref {
	struct power_ref *next;
	ACPI_HANDLE device;
	unsigned int uses;
};
struct acpi_power_res {
	struct acpi_power_res *next, *prev;
	ACPI_HANDLE handle;
	unsigned int order;
	struct power_ref *refs;
};
struct power_device {
	struct power_device *next;
	ACPI_HANDLE handle;
	int state;
	bool seeded, failed;
};
struct power_set {
	struct power_set *next;
	struct acpi_power_res *res;
};

/* Serialize AML and ownership together, never with a spin mutex. */
static kmutex_t power_lock;
static ONCE_DECL(power_once);
static struct acpi_power_res *power_resources, *power_last;
static struct power_device *power_devices;
static bool power_discovered;
static const char *acpi_xname(ACPI_HANDLE);

static int
power_init(void)
{
	mutex_init(&power_lock, MUTEX_DEFAULT, IPL_NONE);
	return 0;
}

static void
power_trace(ACPI_HANDLE handle, const char *method, ACPI_STATUS status,
    ACPI_STATUS sta_status, ACPI_INTEGER sta)
{
	aprint_debug("acpi power: %s %s: %s; post-_STA: %s (%llu)\n",
	    acpi_xname(handle), method, AcpiFormatException(status),
	    AcpiFormatException(sta_status), (unsigned long long)sta);
}

static ACPI_STATUS
power_sta(struct acpi_power_res *res, ACPI_INTEGER *value)
{
	ACPI_STATUS rv;

	*value = 0;
	rv = acpi_eval_integer(res->handle, "_STA", value);
	if (ACPI_SUCCESS(rv) && *value > 1)
		rv = AE_BAD_VALUE;
	power_trace(res->handle, "_STA", rv, rv, *value);
	return rv;
}

/* A failing AML method may act, so collect post-_STA even on failure. */
static ACPI_STATUS
power_method(struct acpi_power_res *res, bool on)
{
	ACPI_INTEGER sta;
	ACPI_STATUS rv, sr;
	const char *method = on ? "_ON" : "_OFF";

	rv = AcpiEvaluateObject(res->handle, method, NULL, NULL);
	sr = power_sta(res, &sta);
	power_trace(res->handle, method, rv, sr, sta);
	if (ACPI_FAILURE(rv))
		return rv;
	if (ACPI_FAILURE(sr))
		return sr;
	return sta == (on ? 1U : 0U) ? AE_OK : AE_ERROR;
}

static ACPI_STATUS
power_resource(ACPI_HANDLE handle, struct acpi_power_res **result)
{
	struct acpi_power_res *res, *at, *prev = NULL;
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *obj;
	ACPI_STATUS rv;

	for (res = power_resources; res != NULL; res = res->next) {
		if (res->handle == handle) {
			*result = res;
			return AE_OK;
		}
	}
	rv = acpi_eval_struct(handle, NULL, &buf);
	if (ACPI_FAILURE(rv))
		goto out;
	obj = buf.Pointer;
	if (obj == NULL || obj->Type != ACPI_TYPE_POWER) {
		rv = AE_TYPE;
		goto out;
	}
	res = kmem_zalloc(sizeof(*res), KM_SLEEP);
	res->handle = handle;
	res->order = obj->PowerResource.ResourceOrder;
	for (at = power_resources; at != NULL && at->order <= res->order;
	    at = at->next)
		prev = at;
	res->prev = prev;
	res->next = at;
	if (prev != NULL)
		prev->next = res;
	else
		power_resources = res;
	if (at != NULL)
		at->prev = res;
	else
		power_last = res;
	*result = res;
out:
	if (buf.Pointer != NULL)
		ACPI_FREE(buf.Pointer);
	return rv;
}

static struct power_ref *
power_reference(struct acpi_power_res *res, ACPI_HANDLE device, bool create)
{
	struct power_ref *ref;

	for (ref = res->refs; ref != NULL; ref = ref->next)
		if (ref->device == device)
			return ref;
	if (!create)
		return NULL;
	ref = kmem_zalloc(sizeof(*ref), KM_SLEEP);
	ref->device = device;
	ref->next = res->refs;
	res->refs = ref;
	return ref;
}

static ACPI_STATUS
power_acquire(struct acpi_power_res *res, ACPI_HANDLE device, unsigned int use)
{
	bool first = res->refs == NULL;
	struct power_ref *ref;
	ACPI_INTEGER sta;
	ACPI_STATUS rv;

	/* Retain the hold on failure: AML may have partly powered on. */
	ref = power_reference(res, device, true);
	ref->uses |= use;
	if (first)
		return power_method(res, true);
	rv = power_sta(res, &sta);
	if (ACPI_FAILURE(rv))
		return rv;
	/* Recovery after uncertain _OFF or externally lost power. */
	return sta == 0 ? power_method(res, true) : AE_OK;
}

static ACPI_STATUS
power_release(struct acpi_power_res *res, ACPI_HANDLE device, unsigned int use)
{
	struct power_ref **link, *ref;
	ACPI_STATUS rv;
	ACPI_INTEGER sta;

	for (link = &res->refs; (ref = *link) != NULL; link = &ref->next)
		if (ref->device == device)
			break;
	if (ref == NULL || (ref->uses & use) == 0)
		return AE_OK;
	if ((ref->uses & ~use) == 0 && ref == res->refs && ref->next == NULL) {
		rv = power_method(res, false);
		if (ACPI_FAILURE(rv))
			return rv;
	} else {
		rv = power_sta(res, &sta);
		if (ACPI_FAILURE(rv))
			return rv;
		if (sta != 1)
			return AE_ERROR;
	}
	ref->uses &= ~use;
	if (ref->uses == 0) {
		*link = ref->next;
		kmem_free(ref, sizeof(*ref));
	}
	return AE_OK;
}

static bool
power_contains(struct power_set *set, struct acpi_power_res *res)
{
	for (; set != NULL; set = set->next)
		if (set->res == res)
			return true;
	return false;
}

static void
power_set_free(struct power_set *set)
{
	struct power_set *next;

	while (set != NULL) {
		next = set->next;
		kmem_free(set, sizeof(*set));
		set = next;
	}
}

/* Absent and empty _PRn are valid; evaluation/type errors are not. */
static ACPI_STATUS
power_package(ACPI_HANDLE handle, int state, struct power_set **set)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *obj;
	ACPI_HANDLE resource;
	struct acpi_power_res *res;
	struct power_set *entry;
	ACPI_STATUS rv;
	uint32_t i;
	char method[] = "_PR0";

	method[3] += state;
	rv = AcpiGetHandle(handle, method, &resource);
	if (rv == AE_NOT_FOUND)
		return AE_OK;
	if (ACPI_FAILURE(rv))
		return rv;
	rv = acpi_eval_struct(handle, method, &buf);
	if (ACPI_FAILURE(rv))
		goto out;
	obj = buf.Pointer;
	if (obj == NULL || obj->Type != ACPI_TYPE_PACKAGE) {
		rv = AE_TYPE;
		goto out;
	}
	for (i = 0; i < obj->Package.Count; i++) {
		rv = acpi_eval_reference_handle(&obj->Package.Elements[i], &resource);
		if (ACPI_FAILURE(rv))
			break;
		rv = power_resource(resource, &res);
		if (ACPI_FAILURE(rv))
			break;
		if (power_contains(*set, res))
			continue;
		entry = kmem_zalloc(sizeof(*entry), KM_SLEEP);
		entry->res = res;
		entry->next = *set;
		*set = entry;
	}
out:
	if (buf.Pointer != NULL)
		ACPI_FREE(buf.Pointer);
	return rv;
}

static struct power_device *
power_device(ACPI_HANDLE handle)
{
	struct power_device *dev;

	for (dev = power_devices; dev != NULL; dev = dev->next)
		if (dev->handle == handle)
			return dev;
	dev = kmem_zalloc(sizeof(*dev), KM_SLEEP);
	dev->handle = handle;
	dev->state = ACPI_STATE_ERROR;
	dev->next = power_devices;
	power_devices = dev;
	return dev;
}

/* Distinguish an absent optional object from an error inside its AML. */
static ACPI_STATUS
power_integer(ACPI_HANDLE handle, const char *method, ACPI_INTEGER *value)
{
	ACPI_HANDLE object;
	ACPI_STATUS rv;

	rv = AcpiGetHandle(handle, method, &object);
	if (ACPI_FAILURE(rv))
		return rv;
	rv = acpi_eval_integer(handle, method, value);
	return rv == AE_NOT_FOUND ? AE_ERROR : rv;
}

static ACPI_STATUS
power_psc(struct power_device *dev, int *state)
{
	ACPI_INTEGER value;
	ACPI_STATUS rv;
	struct acpi_devnode *ad;

	rv = power_integer(dev->handle, "_PSC", &value);
	if (rv == AE_NOT_FOUND) {
		/* Boot's recorded D0 is useful; shared rail state alone is not. */
		ad = acpi_match_node(dev->handle);
		if (ad != NULL && ad->ad_state >= ACPI_STATE_D0 &&
		    ad->ad_state <= ACPI_STATE_D3_COLD) {
			*state = ad->ad_state;
			return AE_OK;
		}
	}
	if (ACPI_FAILURE(rv))
		return rv;
	if (value > ACPI_STATE_D3)
		return AE_BAD_VALUE;
	*state = (int)value;
	return AE_OK;
}

/*
 * Reserve initial ownership without _ON. Unknown states reserve the union
 * of powered _PR0.._PR3 resources; only the owner's transition releases it.
 * Explicitly absent/disabled nodes must not pin shared resources forever.
 */
static ACPI_STATUS
power_seed(struct power_device *dev)
{
	struct power_set *set = NULL, *entry;
	struct power_ref *ref;
	ACPI_INTEGER sta;
	ACPI_STATUS rv;
	int state, first, last;
	bool cold = false;

	if (dev->seeded)
		return AE_OK;
	rv = power_integer(dev->handle, "_STA", &sta);
	if (ACPI_SUCCESS(rv) && (sta & 3) != 3) {
		/* Recheck on registration or a later explicit transition. */
		return AE_OK;
	}
	if (ACPI_FAILURE(rv) && rv != AE_NOT_FOUND)
		return rv;
	rv = power_psc(dev, &state);
	if (ACPI_SUCCESS(rv)) {
		dev->state = state;
		first = state;
		last = state == ACPI_STATE_D3_COLD ? ACPI_STATE_D3 : state;
	} else {
		/* A broken/missing _PSC must not expose another device's rails. */
		first = ACPI_STATE_D0;
		last = ACPI_STATE_D3;
	}
	for (state = first; state <= last; state++) {
		rv = power_package(dev->handle, state, &set);
		if (ACPI_FAILURE(rv))
			goto out;
	}
	for (entry = set; entry != NULL; entry = entry->next) {
		rv = power_sta(entry->res, &sta);
		if (ACPI_FAILURE(rv))
			goto out;
		if (sta == 0) {
			/* A boot D0 record cannot override an unpowered _PR0. */
			if (first == ACPI_STATE_D0 && last == ACPI_STATE_D0)
				cold = true;
			continue;
		}
		ref = power_reference(entry->res, dev->handle, true);
		ref->uses |= POWER_STATE;
	}
	if (cold)
		dev->state = ACPI_STATE_D3_COLD;
	dev->seeded = true;
	rv = AE_OK;
out:
	power_set_free(set);
	return rv;
}

static ACPI_STATUS
power_discover_one(ACPI_HANDLE handle, UINT32 level, void *context, void **ret)
{
	ACPI_HANDLE object;
	ACPI_STATUS rv;
	char method[] = "_PR0";

	(void)level;
	(void)context;
	(void)ret;
	/* Unrelated battery/EC _STA failures cannot affect resource ownership. */
	for (method[3] = '0'; method[3] <= '3'; method[3]++) {
		rv = AcpiGetHandle(handle, method, &object);
		if (ACPI_SUCCESS(rv))
			return power_seed(power_device(handle));
		if (rv != AE_NOT_FOUND)
			return rv;
	}
	return AE_OK;
}

static ACPI_STATUS
power_discover(void)
{
	ACPI_STATUS rv;

	if (power_discovered)
		return AE_OK;
	rv = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
	    ACPI_UINT32_MAX, power_discover_one, NULL, NULL, NULL);
	if (ACPI_SUCCESS(rv))
		power_discovered = true;
	return rv;
}

bool
acpi_power_register(ACPI_HANDLE handle)
{
	ACPI_STATUS rv;

	if (handle == NULL)
		return false;
	RUN_ONCE(&power_once, power_init);
	mutex_enter(&power_lock);
	rv = power_seed(power_device(handle));
	mutex_exit(&power_lock);
	return ACPI_SUCCESS(rv);
}

void
acpi_power_deregister(ACPI_HANDLE handle)
{
	struct acpi_power_res *res;
	struct power_device **link, *dev;
	ACPI_STATUS rv;

	if (handle == NULL)
		return;
	RUN_ONCE(&power_once, power_init);
	mutex_enter(&power_lock);
	rv = power_discover();
	if (ACPI_FAILURE(rv))
		goto out;
	for (res = power_last; res != NULL; res = res->prev) {
		rv = power_release(res, handle, POWER_STATE | POWER_WAKE);
		if (ACPI_FAILURE(rv))
			goto out; /* Keep the record/remaining holds for a retry. */
	}
	for (link = &power_devices; (dev = *link) != NULL; link = &dev->next) {
		if (dev->handle != handle)
			continue;
		*link = dev->next;
		kmem_free(dev, sizeof(*dev));
		break;
	}
out:
	if (ACPI_FAILURE(rv)) {
		dev = power_device(handle);
		dev->state = ACPI_STATE_ERROR;
		dev->failed = true;
		aprint_debug("acpi power: %s deregister failed: %s\n",
		    acpi_xname(handle), AcpiFormatException(rv));
	}
	mutex_exit(&power_lock);
}

bool
acpi_power_get(ACPI_HANDLE handle, int *state)
{
	struct power_device *dev;
	struct power_set *set = NULL, *entry;
	struct acpi_devnode *ad;
	ACPI_INTEGER sta;
	ACPI_STATUS rv;
	int current = ACPI_STATE_ERROR;
	bool cold = false;

	if (handle == NULL) {
		if (state != NULL)
			*state = ACPI_STATE_ERROR;
		return false;
	}
	RUN_ONCE(&power_once, power_init);
	mutex_enter(&power_lock);
	dev = power_device(handle);
	rv = power_seed(dev);
	if (ACPI_FAILURE(rv))
		goto out;
	rv = power_package(handle, ACPI_STATE_D0, &set);
	if (ACPI_FAILURE(rv))
		goto out;
	for (entry = set; entry != NULL; entry = entry->next) {
		rv = power_sta(entry->res, &sta);
		if (ACPI_FAILURE(rv))
			goto out;
		if (sta == 0)
			cold = true;
	}
	if (cold) {
		current = ACPI_STATE_D3_COLD;
		/* Another consumer's _ON cannot resume this device. */
		dev->state = current;
	} else if (dev->failed)
		rv = AE_ERROR;
	else if (dev->state != ACPI_STATE_ERROR)
		current = dev->state;
	else
		rv = power_psc(dev, &current);
	/* Never derive D0 from a shared resource's _STA. */
out:
	if (ACPI_FAILURE(rv))
		current = ACPI_STATE_ERROR;
	if (state != NULL)
		*state = current;
	ad = acpi_match_node(handle);
	if (ad != NULL)
		ad->ad_state = current;
	power_set_free(set);
	mutex_exit(&power_lock);
	return ACPI_SUCCESS(rv);
}

static ACPI_STATUS
power_ps(struct power_device *dev, int state)
{
	ACPI_STATUS rv;
	ACPI_HANDLE object;
	char method[] = "_PS0";

	method[3] += state == ACPI_STATE_D3_COLD ? ACPI_STATE_D3 : state;
	rv = AcpiGetHandle(dev->handle, method, &object);
	if (rv == AE_NOT_FOUND)
		return AE_OK;
	if (ACPI_FAILURE(rv))
		return rv;
	rv = AcpiEvaluateObject(dev->handle, method, NULL, NULL);
	aprint_debug("acpi power: %s %s: %s\n",
	    acpi_xname(dev->handle), method, AcpiFormatException(rv));
	return rv;
}

bool
acpi_power_set(ACPI_HANDLE handle, int state)
{
	struct power_device *dev;
	struct acpi_power_res *res;
	struct power_set *target = NULL;
	struct acpi_devnode *ad;
	ACPI_STATUS rv;

	if (handle == NULL || state < ACPI_STATE_D0 || state > ACPI_STATE_D3_COLD)
		return false;
	RUN_ONCE(&power_once, power_init);
	mutex_enter(&power_lock);
	dev = power_device(handle);
	rv = power_discover();
	if (ACPI_FAILURE(rv))
		goto out;
	rv = power_seed(dev);
	if (ACPI_FAILURE(rv))
		goto out;
	/* A child cannot request more power than its powered parent bus. */
	ad = acpi_match_node(handle);
	if (ad != NULL && ad->ad_parent != NULL &&
	    (ad->ad_parent->ad_flags & ACPI_DEVICE_POWER) != 0 &&
	    ad->ad_parent->ad_state > state) {
		rv = AE_ABORT_METHOD;
		goto out;
	}
	if (state != ACPI_STATE_D3_COLD) {
		rv = power_package(handle, state, &target);
		if (ACPI_FAILURE(rv))
			goto out;
	}
	/* Acquire in ResourceOrder without removing any old ownership. */
	for (res = power_resources; res != NULL; res = res->next) {
		if (!power_contains(target, res))
			continue;
		rv = power_acquire(res, handle, POWER_STATE);
		if (ACPI_FAILURE(rv))
			goto out;
	}
	rv = power_ps(dev, state);
	if (ACPI_FAILURE(rv))
		goto out;
	/* Cold executes _PS3 first; wake holds and common resources survive. */
	for (res = power_last; res != NULL; res = res->prev) {
		if (power_contains(target, res))
			continue;
		rv = power_release(res, handle, POWER_STATE);
		if (ACPI_FAILURE(rv))
			goto out;
	}
	dev->state = state;
	dev->failed = false;
out:
	if (ACPI_FAILURE(rv)) {
		dev->state = ACPI_STATE_ERROR;
		dev->failed = true;
		aprint_debug("acpi power: %s D%d failed: %s\n",
		    acpi_xname(handle), state, AcpiFormatException(rv));
	}
	ad = acpi_match_node(handle);
	if (ad != NULL)
		ad->ad_state = dev->state;
	power_set_free(target);
	mutex_exit(&power_lock);
	return ACPI_SUCCESS(rv);
}

ACPI_STATUS
acpi_power_res(ACPI_HANDLE handle, ACPI_HANDLE device, bool on)
{
	struct acpi_power_res *res;
	ACPI_STATUS rv;

	if (handle == NULL || device == NULL)
		return AE_BAD_PARAMETER;
	RUN_ONCE(&power_once, power_init);
	mutex_enter(&power_lock);
	rv = power_discover();
	if (ACPI_FAILURE(rv))
		goto out;
	rv = power_resource(handle, &res);
	if (ACPI_SUCCESS(rv))
		rv = on ? power_acquire(res, device, POWER_WAKE) :
		    power_release(res, device, POWER_WAKE);
out:
	mutex_exit(&power_lock);
	return rv;
}

#ifndef ACPI_POWER_TEST
static int32_t acpi_power_acpinode = CTL_EOL;
static int32_t acpi_power_powernode = CTL_EOL;
static int acpi_power_sysctl(SYSCTLFN_PROTO);

SYSCTL_SETUP(sysctl_acpi_power_setup, "sysctl hw.acpi.power subtree setup")
{
	const struct sysctlnode *anode;
	int err;

	err = sysctl_createv(NULL, 0, NULL, &anode,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "acpi",
	    NULL, NULL, 0, NULL, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);

	if (err != 0)
		return;

	acpi_power_acpinode = anode->sysctl_num;

	err = sysctl_createv(NULL, 0, &anode, &anode,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE,
	    "power", SYSCTL_DESCR("ACPI device power states"),
	    NULL, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	if (err != 0)
		return;

	acpi_power_powernode = anode->sysctl_num;
}

void
acpi_power_add(struct acpi_devnode *ad)
{
	const char *str = NULL;
	int err;

	KASSERT(ad != NULL && ad->ad_root != NULL);
	KASSERT((ad->ad_flags & ACPI_DEVICE_POWER) != 0);

	/* Include hot-added and driverless resource owners before display setup. */
	if (!acpi_power_register(ad->ad_handle))
		aprint_debug("acpi power: %s initial ownership unavailable\n",
		    ad->ad_name);

	if (acpi_power_acpinode == CTL_EOL ||
	    acpi_power_powernode == CTL_EOL)
		return;

	if (ad->ad_device != NULL)
		str = device_xname(ad->ad_device);
#if NPCI > 0
	else {
		device_t dev = acpi_pcidev_find_dev(ad);

		if (dev != NULL)
			str = device_xname(dev);
	}
#endif

	if (str == NULL)
		return;

	err = sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_READONLY, CTLTYPE_STRING, str,
	    NULL, acpi_power_sysctl, 0, (void *)ad, 0, CTL_HW,
	    acpi_power_acpinode, acpi_power_powernode,
	    CTL_CREATE, CTL_EOL);

	if (err != 0)
		aprint_error_dev(ad->ad_root, "sysctl_createv"
		    "(hw.acpi.power.%s) failed (err %d)\n", str, err);
}

static int
acpi_power_sysctl(SYSCTLFN_ARGS)
{
	struct acpi_devnode *ad;
	struct sysctlnode node;
	int err, state;
	char t[8];

	node = *rnode;
	ad = rnode->sysctl_data;

	if (acpi_power_get(ad->ad_handle, &state) != true)
		state = ACPI_STATE_ERROR;

	(void)memset(t, '\0', sizeof(t));
	(void)snprintf(t, sizeof(t), "D%d", state);

	node.sysctl_data = &t;

	err = sysctl_lookup(SYSCTLFN_CALL(&node));

	if (err || newp == NULL)
		return err;

	return 0;
}
#endif /* !ACPI_POWER_TEST */

/*
 * XXX: Move this to acpi_util.c by refactoring
 *	acpi_name() to optionally return a single name.
 */
static const char *
acpi_xname(ACPI_HANDLE hdl)
{
	static char str[5];
	ACPI_BUFFER buf;
	ACPI_STATUS rv;

	buf.Pointer = str;
	buf.Length = sizeof(str);

	rv = AcpiGetName(hdl, ACPI_SINGLE_NAME, &buf);

	if (ACPI_FAILURE(rv))
		return "????";

	return str;
}
