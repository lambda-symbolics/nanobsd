/* Host ACPICA adapters; the state/resource algorithm is production C below. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

typedef void *ACPI_HANDLE;
typedef uint64_t ACPI_INTEGER;
typedef uint32_t UINT32;
typedef int ACPI_STATUS;
typedef struct { size_t Length; void *Pointer; } ACPI_BUFFER;
typedef union acpi_object ACPI_OBJECT;
union acpi_object {
	unsigned int Type;
	struct { unsigned int Type, Count; ACPI_OBJECT *Elements; } Package;
	struct { unsigned int Type, SystemLevel, ResourceOrder; } PowerResource;
	struct { unsigned int Type; ACPI_HANDLE Handle; } Reference;
};
#define AE_OK 0
#define AE_ERROR 1
#define AE_NOT_FOUND 2
#define AE_TYPE 3
#define AE_BAD_VALUE 4
#define AE_BAD_PARAMETER 5
#define AE_ABORT_METHOD 6
#define ACPI_SUCCESS(x) ((x) == AE_OK)
#define ACPI_FAILURE(x) ((x) != AE_OK)
#define ACPI_TYPE_DEVICE 6
#define ACPI_TYPE_POWER 11
#define ACPI_TYPE_PACKAGE 4
#define ACPI_TYPE_LOCAL_REFERENCE 20
#define ACPI_STATE_D0 0
#define ACPI_STATE_D1 1
#define ACPI_STATE_D2 2
#define ACPI_STATE_D3 3
#define ACPI_ROOT_OBJECT NULL
#define ACPI_UINT32_MAX UINT32_MAX
#define ACPI_ALLOCATE_BUFFER ((size_t)-1)
#define ACPI_SINGLE_NAME 1
#define ACPI_FREE free
#define ACPI_BUS_COMPONENT 0
#define ACPI_MODULE_NAME(x)
#define ACPI_DEVICE_POWER 1U
#define KM_SLEEP 0
#define MUTEX_DEFAULT 0
#define IPL_NONE 0
#define ACPI_POWER_TEST 1

typedef pthread_mutex_t kmutex_t;
#define ONCE_DECL(name) int name
#define RUN_ONCE(once, fn) do { if (!*(once)) { assert((fn)() == 0); *(once) = 1; } } while (0)
static void mutex_init(kmutex_t *m, int type, int ipl)
{ (void)type; (void)ipl; assert(pthread_mutex_init(m, NULL) == 0); }
static void mutex_enter(kmutex_t *m) { assert(pthread_mutex_lock(m) == 0); }
static void mutex_exit(kmutex_t *m) { assert(pthread_mutex_unlock(m) == 0); }
static void *kmem_zalloc(size_t size, int flags)
{ void *p = calloc(1, size); (void)flags; assert(p); return p; }
static void kmem_free(void *p, size_t size) { (void)size; free(p); }
static void aprint_debug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (getenv("ACPI_POWER_TRACE")) vfprintf(stderr, fmt, ap);
	va_end(ap);
}
static const char *AcpiFormatException(ACPI_STATUS status)
{
	static const char *const names[] = { "OK", "ERROR", "NOT_FOUND", "TYPE", "BAD_VALUE", "BAD_PARAMETER", "ABORT" };
	return names[status];
}
struct acpi_devnode {
	int ad_state;
	unsigned int ad_flags;
	struct acpi_devnode *ad_parent;
};
#include "../kernel/dev/acpi/acpi_power.h"

#define MAX_OBJECTS 40
#define MAX_PACKAGE 8
struct fake_object {
	char name[5];
	bool resource, attached, sta_present, psc_present;
	unsigned int order;
	ACPI_INTEGER sta;
	int psc;
	ACPI_STATUS sta_error, psc_error, on_error, off_error;
	unsigned int sta_calls, sta_fail_at;
	bool on_mismatch, off_mismatch, sta_invalid;
	bool pr_present[4], ps_present[4];
	ACPI_STATUS pr_error[4], ps_error[4];
	unsigned int pr_type[4], npr[4];
	struct fake_object *pr[4][MAX_PACKAGE];
	unsigned int on_calls, off_calls;
	struct acpi_devnode ad;
};
static struct fake_object objects[MAX_OBJECTS];
static unsigned int nobjects;
static char events[4096];
static size_t nevents;
static void event(struct fake_object *o, const char *method)
{
	int n = snprintf(events + nevents, sizeof(events) - nevents, "%s%s ", o->name, method);
	assert(n >= 0 && (size_t)n < sizeof(events) - nevents);
	nevents += (size_t)n;
}
static struct acpi_devnode *acpi_match_node(ACPI_HANDLE h)
{ struct fake_object *o = h; return o->attached ? &o->ad : NULL; }
static ACPI_STATUS AcpiGetName(ACPI_HANDLE h, int type, ACPI_BUFFER *b)
{ struct fake_object *o = h; (void)type; assert(b->Length >= 5); memcpy(b->Pointer, o->name, 5); return AE_OK; }
static ACPI_STATUS AcpiGetHandle(ACPI_HANDLE h, const char *name, ACPI_HANDLE *out)
{
	struct fake_object *o = h;
	bool present = false;
	if (!strcmp(name, "_STA")) present = o->sta_present;
	else if (!strcmp(name, "_PSC")) present = o->psc_present;
	else if (!strncmp(name, "_PR", 3)) present = o->pr_present[name[3] - '0'];
	else if (!strncmp(name, "_PS", 3)) present = o->ps_present[name[3] - '0'];
	if (!present) return AE_NOT_FOUND;
	*out = h;
	return AE_OK;
}
static ACPI_STATUS acpi_eval_integer(ACPI_HANDLE h, const char *name, ACPI_INTEGER *v)
{
	struct fake_object *o = h;
	if (!strcmp(name, "_STA")) {
		if (!o->sta_present) return AE_NOT_FOUND;
		o->sta_calls++;
		if (o->sta_error) return o->sta_error;
		if (o->sta_fail_at && o->sta_calls == o->sta_fail_at) return AE_ERROR;
		*v = o->sta_invalid ? 7 : o->sta;
	} else {
		assert(!strcmp(name, "_PSC"));
		if (!o->psc_present) return AE_NOT_FOUND;
		if (o->psc_error) return o->psc_error;
		*v = (ACPI_INTEGER)o->psc;
	}
	return AE_OK;
}
static ACPI_STATUS acpi_eval_struct(ACPI_HANDLE h, const char *name, ACPI_BUFFER *b)
{
	struct fake_object *o = h;
	ACPI_OBJECT *obj;
	unsigned int n = 0, i, state = 0;
	b->Pointer = NULL;
	if (name != NULL) {
		state = (unsigned int)(name[3] - '0');
		assert(state < 4);
		if (!o->pr_present[state]) return AE_NOT_FOUND;
		if (o->pr_error[state]) return o->pr_error[state];
		n = o->npr[state];
	}
	obj = calloc(n + 1, sizeof(*obj));
	assert(obj);
	b->Pointer = obj;
	b->Length = (n + 1) * sizeof(*obj);
	if (name == NULL) {
		obj->Type = o->resource ? ACPI_TYPE_POWER : ACPI_TYPE_DEVICE;
		obj->PowerResource.ResourceOrder = o->order;
	} else {
		obj->Type = o->pr_type[state];
		obj->Package.Count = n;
		obj->Package.Elements = obj + 1;
		for (i = 0; i < n; i++) {
			obj[i + 1].Type = ACPI_TYPE_LOCAL_REFERENCE;
			obj[i + 1].Reference.Handle = o->pr[state][i];
		}
	}
	return AE_OK;
}
static ACPI_STATUS acpi_eval_reference_handle(ACPI_OBJECT *o, ACPI_HANDLE *h)
{
	if (o->Type != ACPI_TYPE_LOCAL_REFERENCE || !o->Reference.Handle) return AE_TYPE;
	*h = o->Reference.Handle;
	return AE_OK;
}
static ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE h, const char *name, void *args, void *out)
{
	struct fake_object *o = h;
	(void)args; (void)out;
	event(o, name);
	if (!strcmp(name, "_ON")) {
		o->on_calls++;
		if (o->on_error) return o->on_error;
		if (!o->on_mismatch) o->sta = 1;
	} else if (!strcmp(name, "_OFF")) {
		o->off_calls++;
		if (o->off_error) return o->off_error;
		if (!o->off_mismatch) o->sta = 0;
	} else {
		int state = name[3] - '0';
		assert(state >= 0 && state <= 3 && !strncmp(name, "_PS", 3));
		if (!o->ps_present[state]) return AE_NOT_FOUND;
		if (o->ps_error[state]) return o->ps_error[state];
		o->psc = state;
	}
	return AE_OK;
}
typedef ACPI_STATUS (*walk_callback)(ACPI_HANDLE, UINT32, void *, void **);
static ACPI_STATUS AcpiWalkNamespace(unsigned int type, ACPI_HANDLE root, UINT32 depth,
    walk_callback down, walk_callback up, void *context, void **ret)
{
	unsigned int i;
	(void)root; (void)depth; (void)up;
	assert(type == ACPI_TYPE_DEVICE);
	for (i = 0; i < nobjects; i++) {
		ACPI_STATUS rv;
		if (objects[i].resource) continue;
		rv = down(&objects[i], 0, context, ret);
		if (ACPI_FAILURE(rv)) return rv;
	}
	return AE_OK;
}

#include "../kernel/dev/acpi/acpi_power.c"

static void reset(void)
{
	while (power_resources) {
		struct acpi_power_res *r = power_resources;
		while (r->refs) {
			struct power_ref *ref = r->refs;
			r->refs = ref->next;
			free(ref);
		}
		power_resources = r->next;
		free(r);
	}
	while (power_devices) {
		struct power_device *d = power_devices;
		power_devices = d->next;
		free(d);
	}
	power_last = NULL;
	power_discovered = false;
	memset(objects, 0, sizeof(objects));
	nobjects = 0;
	nevents = 0;
	events[0] = 0;
}
static void clear_events(void) { nevents = 0; events[0] = 0; }
static struct fake_object *object(const char *name, bool resource, int sta)
{
	struct fake_object *o;
	unsigned int i;
	assert(nobjects < MAX_OBJECTS && strlen(name) <= 4);
	o = &objects[nobjects++];
	strcpy(o->name, name);
	o->resource = resource;
	o->attached = !resource;
	o->sta_present = true;
	o->sta = (ACPI_INTEGER)sta;
	o->psc_present = !resource;
	o->ad.ad_flags = ACPI_DEVICE_POWER;
	for (i = 0; i < 4; i++) {
		o->ps_present[i] = !resource;
		o->pr_type[i] = ACPI_TYPE_PACKAGE;
	}
	return o;
}
static void package(struct fake_object *d, int state, struct fake_object *r)
{
	unsigned int n = d->npr[state]++;
	assert(n < MAX_PACKAGE);
	d->pr_present[state] = true;
	d->pr[state][n] = r;
}
static unsigned int holds(struct fake_object *r)
{
	struct acpi_power_res *res;
	struct power_ref *ref;
	unsigned int n = 0;
	for (res = power_resources; res && res->handle != r; res = res->next) {}
	if (!res) return 0;
	for (ref = res->refs; ref; ref = ref->next) n++;
	return n;
}
static void common_resources(void)
{
	struct fake_object *r, *a;
	int state;
	reset(); r = object("RA", true, 1); a = object("A", false, 15);
	package(a, 0, r); package(a, 3, r); package(a, 0, r); /* duplicate */
	assert(acpi_power_get(a, &state) && state == 0);
	assert(holds(r) == 1 && r->on_calls == 0);
	assert(acpi_power_set(a, 3));
	assert(acpi_power_get(a, &state) && state == 3);
	assert(acpi_power_set(a, 0));
	assert(!r->on_calls && !r->off_calls && holds(r) == 1);
	assert(!strcmp(events, "A_PS3 A_PS0 "));
	r->sta = 0;
	assert(acpi_power_get(a, &state) && state == ACPI_STATE_D3_COLD);
	puts("ok common resources, duplicates, recorded hot state and physical cold");
}
static void shared_and_untouched(void)
{
	struct fake_object *r, *a, *b;
	reset(); r = object("RA", true, 1); a = object("A", false, 15); b = object("B", false, 15);
	package(a, 0, r); package(b, 0, r);
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(holds(r) == 1 && !r->off_calls && !r->on_calls);
	assert(acpi_power_set(a, 0));
	assert(holds(r) == 2 && !r->off_calls && !r->on_calls);
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(!r->off_calls);
	assert(acpi_power_set(b, ACPI_STATE_D3_COLD));
	assert(r->off_calls == 1 && holds(r) == 0);
	puts("ok untouched sibling, shared cold/restore, last consumer release");
}
static void dedicated_order(void)
{
	struct fake_object *lo, *hi, *a;
	reset(); hi = object("HI", true, 1); hi->order = 50;
	lo = object("LO", true, 1); lo->order = 10; a = object("A", false, 15);
	package(a, 0, hi); package(a, 0, lo);
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(!strcmp(events, "A_PS3 HI_OFF LO_OFF "));
	clear_events();
	assert(acpi_power_set(a, 0));
	assert(!strcmp(events, "LO_ON HI_ON A_PS0 "));
	puts("ok dedicated cold/restore order and ResourceOrder");
}
static void acquisition_failure(void)
{
	struct fake_object *old, *new, *a;
	reset(); old = object("OLD", true, 1); new = object("NEW", true, 0); a = object("A", false, 15);
	package(a, 0, old); package(a, 3, new);
	new->on_error = AE_ERROR;
	assert(!acpi_power_set(a, 3));
	assert(a->ad.ad_state == ACPI_STATE_ERROR && old->off_calls == 0 && holds(old) == 1);
	assert(!strcmp(events, "NEW_ON "));
	new->on_error = AE_OK;
	assert(acpi_power_set(a, 0));
	assert(a->ad.ad_state == 0 && holds(old) == 1 && holds(new) == 0);
	puts("ok failed acquisition retains old ownership and permits recovery");
}
static void method_failures(void)
{
	struct fake_object *r, *a;
	unsigned int which;
	for (which = 0; which < 7; which++) {
		reset(); r = object("RA", true, 1); a = object("A", false, 15); package(a, 0, r);
		assert(acpi_power_register(a));
		switch (which) {
		case 0: r->off_error = AE_ERROR; break;
		case 1: a->ps_error[3] = AE_ERROR; break;
		case 2: r->sta_error = AE_ERROR; break;
		case 3: r->off_mismatch = true; break;
		case 4: r->sta_invalid = true; break;
		case 5: a->ps_error[3] = AE_NOT_FOUND; break;
		case 6: r->sta_fail_at = r->sta_calls + 1; break;
		}
		assert(!acpi_power_set(a, ACPI_STATE_D3_COLD));
		assert(a->ad.ad_state == ACPI_STATE_ERROR && holds(r) == 1);
		if (which == 1 || which == 5) assert(r->off_calls == 0);
	}
	for (which = 0; which < 3; which++) {
		reset(); r = object("RA", true, 0); a = object("A", false, 15); package(a, 0, r);
		assert(acpi_power_register(a));
		if (which == 0) r->on_mismatch = true;
		if (which == 1) r->sta_error = AE_ERROR;
		if (which == 2) r->sta_fail_at = r->sta_calls + 1;
		assert(!acpi_power_set(a, 0));
		assert(a->ad.ad_state == ACPI_STATE_ERROR);
	}
	puts("ok ON/OFF/PS errors, post-STA failures and mismatches");
}
static void many_and_wake(void)
{
	struct fake_object *r, *devices[12];
	unsigned int i;
	reset(); r = object("RA", true, 1);
	for (i = 0; i < 12; i++) {
		char name[5];
		snprintf(name, sizeof(name), "D%u", i);
		devices[i] = object(name, false, 15); package(devices[i], 0, r);
	}
	assert(acpi_power_res(r, devices[0], true) == AE_OK);
	assert(acpi_power_res(r, devices[0], true) == AE_OK);
	assert(holds(r) == 12 && r->on_calls == 0);
	for (i = 0; i < 12; i++) assert(acpi_power_set(devices[i], ACPI_STATE_D3_COLD));
	assert(r->off_calls == 0 && holds(r) == 1);
	assert(acpi_power_res(r, devices[0], false) == AE_OK);
	assert(r->off_calls == 1 && holds(r) == 0);
	assert(acpi_power_res(r, devices[0], false) == AE_OK);
	assert(r->off_calls == 1);
	puts("ok twelve consumers and independent idempotent wake ownership");
}
static void initial_state(void)
{
	struct fake_object *r, *a, *b;
	int state;
	reset(); r = object("RA", true, 1); a = object("A", false, 15); b = object("B", false, 0);
	package(a, 0, r); package(b, 0, r);
	a->psc_present = false;
	assert(acpi_power_get(a, &state) && state == 0 && !r->on_calls);
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(r->off_calls == 1); /* absent sibling did not pin the rail */
	assert(acpi_power_set(a, 0));
	b->sta = 15; /* new arrival after namespace discovery */
	assert(acpi_power_register(b));
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(r->off_calls == 1 && holds(r) == 1);
	acpi_power_deregister(b);
	assert(r->off_calls == 2 && holds(r) == 0);
	reset(); r = object("RA", true, 1); a = object("A", false, 15); b = object("B", false, 15);
	package(a, 0, r); package(b, 0, r); b->attached = false; b->psc_present = false;
	assert(!acpi_power_get(b, &state) && state == ACPI_STATE_ERROR);
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(!r->off_calls && holds(r) == 1);
	puts("ok missing PSC boot state, absent nodes, hot arrival and unknown sibling");
}
static void package_errors(void)
{
	struct fake_object *r, *a;
	unsigned int which;
	for (which = 0; which < 3; which++) {
		reset(); r = object("RA", true, 1); a = object("A", false, 15);
		package(a, 0, r); package(a, 3, r);
		if (which == 0) a->pr_error[3] = AE_NOT_FOUND;
		if (which == 1) a->pr_type[3] = ACPI_TYPE_DEVICE;
		if (which == 2) a->pr[3][0] = NULL;
		assert(!acpi_power_set(a, 3));
		assert(!r->off_calls && holds(r) == 1 && a->ad.ad_state == ACPI_STATE_ERROR);
	}
	puts("ok malformed package/reference and existing AML NOT_FOUND errors");
}
static void edge_cases(void)
{
	struct fake_object *r, *a, *b, *hi;
	int state;
	reset(); r = object("RA", true, 1); a = object("A", false, 15);
	package(a, 0, r);
	b = object("BATT", false, 15); b->sta_error = AE_ERROR;
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(r->off_calls == 1); /* unrelated device STA error is irrelevant */
	reset(); r = object("RA", true, 1); a = object("A", false, 15);
	package(a, 0, r); a->sta_present = false; a->psc_present = false;
	assert(acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(r->off_calls == 1); /* missing device STA means present */
	assert(acpi_power_get(a, &state) && state == ACPI_STATE_D3_COLD);
	r->sta_error = AE_ERROR;
	assert(!acpi_power_get(a, &state) && state == ACPI_STATE_ERROR);
	reset(); a = object("A", false, 15); a->ps_present[3] = false;
	assert(acpi_power_set(a, 3)); /* optional absent methods */
	assert(acpi_power_get(a, &state) && state == 3);
	assert(nevents == 0);
	a->pr_present[0] = true; /* an empty package is valid */
	assert(acpi_power_set(a, 0));
	reset(); r = object("RA", true, 1); a = object("A", false, 15);
	b = object("BUS", false, 15); b->ad.ad_state = 3;
	a->ad.ad_parent = &b->ad; package(a, 0, r);
	assert(!acpi_power_set(a, 0) && !r->on_calls && !r->off_calls);
	reset(); r = object("LO", true, 1); hi = object("HI", true, 1);
	hi->order = 5; a = object("A", false, 15);
	package(a, 0, r); package(a, 0, hi); r->off_error = AE_ERROR;
	assert(!acpi_power_set(a, ACPI_STATE_D3_COLD));
	assert(hi->off_calls == 1 && holds(hi) == 0 && holds(r) == 1);
	r->off_error = AE_OK; clear_events();
	assert(acpi_power_set(a, 0));
	assert(!strcmp(events, "HI_ON A_PS0 "));
	assert(holds(r) == 1 && holds(hi) == 1);
	assert(!acpi_power_get(NULL, &state));
	assert(!acpi_power_set(NULL, 0));
	assert(!acpi_power_register(NULL));
	assert(acpi_power_res(NULL, a, true) == AE_BAD_PARAMETER);
	puts("ok optional methods, unrelated STA failure, parent constraint, partial-OFF recovery");
}
static void overlapping_targets(void)
{
	struct fake_object *common, *old, *new, *a;
	reset(); common = object("COM", true, 1); old = object("OLD", true, 1);
	new = object("NEW", true, 0); a = object("A", false, 15);
	package(a, 0, common); package(a, 0, old);
	package(a, 3, common); package(a, 3, new);
	assert(acpi_power_set(a, 3));
	assert(!strcmp(events, "NEW_ON A_PS3 OLD_OFF "));
	assert(common->on_calls == 0 && common->off_calls == 0 && holds(common) == 1);
	clear_events();
	assert(acpi_power_set(a, 0));
	assert(!strcmp(events, "OLD_ON A_PS0 NEW_OFF "));
	assert(common->on_calls == 0 && common->off_calls == 0 && holds(common) == 1);
	reset(); common = object("COM", true, 0); a = object("A", false, 15);
	assert(acpi_power_res(common, a, true) == AE_OK);
	assert(acpi_power_res(common, a, true) == AE_OK && common->on_calls == 1);
	assert(acpi_power_res(common, a, false) == AE_OK && common->off_calls == 1);
	puts("ok partially overlapping targets and first/last public consumer");
}
static void cold_observation(void)
{
	struct fake_object *r, *a, *b;
	int state;
	unsigned int scenario;

	for (scenario = 0; scenario < 3; scenario++) {
		reset(); r = object("RA", true, scenario == 2 ? 1 : 0);
		a = object("A", false, 15); b = object("B", false, 15);
		package(a, 0, r); package(b, 0, r);
		a->psc_present = b->psc_present = false;
		if (scenario == 2) {
			assert(acpi_power_get(a, &state) && state == 0);
			r->sta = 0; /* Power lost after initial ownership was recorded. */
		}
		if (scenario != 0)
			assert(acpi_power_get(a, &state) && state == ACPI_STATE_D3_COLD);
		assert(acpi_power_set(b, 0));
		/* A sibling's _ON is not this device's _PS0. */
		assert(acpi_power_get(a, &state) && state == ACPI_STATE_D3_COLD);
		assert(acpi_power_set(a, 0));
		assert(acpi_power_get(a, &state) && state == 0);
	}
	puts("ok initial and observed cold state survives sibling resource activation");
}
int main(void)
{
	common_resources(); shared_and_untouched(); dedicated_order();
	acquisition_failure(); method_failures(); many_and_wake();
	initial_state(); package_errors(); edge_cases(); overlapping_targets();
	cold_observation();
	reset();
	puts("all ACPI power production-backend tests passed");
	return 0;
}
