/*	$NetBSD$	*/

/*
 * imt - I2C HID Windows Precision Touchpad, with two-finger scrolling.
 *
 * NetBSD 11 declares an "imt" device in dev/i2c/files.i2c but ships neither
 * dev/i2c/imt.c nor the dev/hid/hidmt.c layer it names, so the device could
 * never be configured.  OpenBSD's hidmt cannot be ported across directly: it
 * is written against OpenBSD's wsmouse multitouch framework
 * (wsmouse_mtstate/wsmouse_id_to_slot/wsmouse_configure/wsmouse_input_sync),
 * none of which exists here.  Replacing wscons' shared mouse layer to gain
 * that framework would be a far larger and riskier change than this driver.
 *
 * Instead this decodes the Precision Touchpad reports directly and drives
 * NetBSD's existing wsmouse interface:
 *
 *	one finger	-> relative pointer motion (wsmouse_input)
 *	two fingers	-> scrolling (wsmouse_precision_scroll, which emits
 *			   real WSCONS_EVENT_[HV]SCROLL events)
 *	button/clickpad	-> button state
 *
 * The pad must first be switched out of its mouse-emulation mode by writing
 * the Precision Touchpad value to the Input Mode feature report; until then
 * it reports as a plain two-button mouse and the multitouch reports stay
 * silent (which is what ims(4) binds to).
 *
 * Report parsing follows OpenBSD's hidmt(4) by joshua stein.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include <dev/i2c/i2cvar.h>
#include <dev/i2c/ihidev.h>

#include <dev/hid/hid.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#define IMT_MAX_CONTACTS	10
#define IMT_INPUT_MODE_PTP	0x03

/*
 * Touchpad coordinates are far finer than screen pixels (a few thousand
 * units across the pad), so raw deltas would send the pointer flying.
 * These divisors are the pointer and scroll sensitivity; they are sysctl
 * tunables because the right value is a matter of taste.
 */
#define IMT_MOTION_DIV_DEFAULT	3
/*
 * Pad units per wheel click.  Scrolling is delivered as z/w axis deltas
 * through wsmouse_input(), NOT via wsmouse_precision_scroll(): the X ws
 * driver here consumes the wheel axes ("ZAxisMapping 4 5 6 7"), which is
 * also how pms(4) delivers the TrackPoint's working middle-button scroll.
 * wsmouse_precision_scroll() emits WSCONS_EVENT_[HV]SCROLL instead, which
 * that path ignores, so two-finger scrolling produced nothing at all.
 */
#define IMT_SCROLL_DIV_DEFAULT	100

/* Ignore absurd jumps, e.g. when a finger is lifted and set down elsewhere. */
#define IMT_JUMP_LIMIT		400

struct imt_contact {
	struct hid_location	loc_tip;
	struct hid_location	loc_id;
	struct hid_location	loc_x;
	struct hid_location	loc_y;
	bool			valid;
};

struct imt_softc {
	struct ihidev		sc_hdev;
	device_t		sc_wsmousedev;
	bool			sc_enabled;

	struct imt_contact	sc_contacts[IMT_MAX_CONTACTS];
	int			sc_nslots;
	struct hid_location	sc_loc_count;
	struct hid_location	sc_loc_btn;
	bool			sc_have_btn;

	int			sc_cfg_rid;	/* Input Mode feature report */

	/* gesture state, carried between reports */
	int			sc_prev_x, sc_prev_y;
	int			sc_prev_down;
	uint32_t		sc_prev_btn;
	/* sub-click scroll remainder, so slow drags still scroll */
	int			sc_acc_x, sc_acc_y;
};

static int	imt_match(device_t, cfdata_t, void *);
static void	imt_attach(device_t, device_t, void *);
static int	imt_detach(device_t, int);
static void	imt_intr(struct ihidev *, void *, u_int);

static int	imt_enable(void *);
static void	imt_disable(void *);
static int	imt_ioctl(void *, u_long, void *, int, struct lwp *);

int imt_debug = 0;
int imt_motion_div = IMT_MOTION_DIV_DEFAULT;
int imt_scroll_div = IMT_SCROLL_DIV_DEFAULT;
int imt_scroll_invert = 0;

static struct sysctllog *imt_sysctllog;

static const struct wsmouse_accessops imt_accessops = {
	imt_enable,
	imt_ioctl,
	imt_disable,
};

CFATTACH_DECL_NEW(imt, sizeof(struct imt_softc), imt_match, imt_attach,
    imt_detach, NULL);

/*
 * Collect the per-contact item locations for one input report.  A Precision
 * Touchpad report is a repeating series of finger collections; each Tip
 * Switch starts a new one.
 */
static int
imt_parse_input(struct imt_softc *sc, const void *desc, int dlen, uint8_t repid)
{
	struct imt_contact *c = NULL;
	struct hid_data *hd;
	struct hid_item h;
	int nslots = 0;

	hd = hid_start_parse(desc, dlen, hid_input);
	if (hd == NULL)
		return 0;
	while (hid_get_item(hd, &h)) {
		if (h.kind != hid_input || h.report_ID != repid)
			continue;
		switch (h.usage) {
		case HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH):
			if (nslots < IMT_MAX_CONTACTS) {
				c = &sc->sc_contacts[nslots++];
				c->loc_tip = h.loc;
				c->valid = true;
			} else {
				c = NULL;
			}
			break;
		case HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTID):
			if (c != NULL)
				c->loc_id = h.loc;
			break;
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X):
			if (c != NULL)
				c->loc_x = h.loc;
			break;
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y):
			if (c != NULL)
				c->loc_y = h.loc;
			break;
		case HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTCOUNT):
			sc->sc_loc_count = h.loc;
			break;
		case HID_USAGE2(HUP_BUTTON, 1):
			sc->sc_loc_btn = h.loc;
			sc->sc_have_btn = true;
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	sc->sc_nslots = nslots;
	return nslots > 0 && sc->sc_loc_count.size > 0;
}

/* Find the feature report carrying Input Mode, which selects PTP mode. */
static int
imt_find_config(const void *desc, int dlen, int *ridp)
{
	struct hid_data *hd;
	struct hid_item h;
	int found = 0;

	hd = hid_start_parse(desc, dlen, hid_feature);
	if (hd == NULL)
		return 0;
	while (hid_get_item(hd, &h)) {
		if (h.kind == hid_feature &&
		    h.usage == HID_USAGE2(HUP_DIGITIZERS, HUD_INPUT_MODE)) {
			*ridp = h.report_ID;
			found = 1;
			break;
		}
	}
	hid_end_parse(hd);
	return found;
}

/*
 * Pointer and scroll sensitivity are a matter of taste, and the useful
 * values depend on the pad's resolution, so expose them rather than baking
 * them in: machdep.imt.motion_div, machdep.imt.scroll_div.
 */
static void
imt_sysctl_setup(void)
{
	const struct sysctlnode *node = NULL;

	if (imt_sysctllog != NULL)
		return;
	sysctl_createv(&imt_sysctllog, 0, NULL, &node, CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "imt", SYSCTL_DESCR("I2C precision touchpad"),
	    NULL, 0, NULL, 0, CTL_MACHDEP, CTL_CREATE, CTL_EOL);
	if (node == NULL)
		return;
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "motion_div",
	    SYSCTL_DESCR("pad units per pointer unit (higher = slower)"),
	    NULL, 0, &imt_motion_div, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "scroll_div",
	    SYSCTL_DESCR("pad units per scroll unit (higher = slower)"),
	    NULL, 0, &imt_scroll_div, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "scroll_invert",
	    SYSCTL_DESCR("reverse the two-finger scroll direction"),
	    NULL, 0, &imt_scroll_invert, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "debug",
	    SYSCTL_DESCR("report what each HID report id contains at attach"),
	    NULL, 0, &imt_debug, 0, CTL_CREATE, CTL_EOL);
}

static int
imt_match(device_t parent, cfdata_t match, void *aux)
{
	struct ihidev_attach_arg *iha = aux;
	struct hid_data *hd;
	struct hid_item h;
	void *desc;
	int size, rid, cfg;
	bool tip = false, cid = false, x = false, y = false;

	ihidev_get_report_desc(iha->parent, &desc, &size);
	rid = iha->reportid;

	/*
	 * Claim only a real Precision Touchpad input report: contact id and
	 * tip switch together are what distinguish it from the plain mouse
	 * report that ims(4) takes.  Also require the Input Mode feature
	 * report, without which the pad cannot be switched into this mode.
	 */
	hd = hid_start_parse(desc, size, hid_input);
	if (hd == NULL)
		return IMATCH_NONE;
	while (hid_get_item(hd, &h)) {
		if (h.kind != hid_input || h.report_ID != rid)
			continue;
		if (h.usage == HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH))
			tip = true;
		else if (h.usage == HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTID))
			cid = true;
		else if (h.usage == HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X))
			x = true;
		else if (h.usage == HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y))
			y = true;
	}
	hid_end_parse(hd);

	cfg = -1;
	(void)imt_find_config(desc, size, &cfg);
	if (imt_debug)
		printf("imt: rid %d: tip=%d cid=%d x=%d y=%d cfg_rid=%d\n",
		    rid, tip, cid, x, y, cfg);

	if (!(tip && cid && x && y))
		return IMATCH_NONE;
	if (cfg < 0)
		return IMATCH_NONE;

	/* Outrank ims(4), which reports IMATCH_IFACECLASS. */
	return IMATCH_IFACECLASS_IFACESUBCLASS;
}

static void
imt_attach(device_t parent, device_t self, void *aux)
{
	struct imt_softc *sc = device_private(self);
	struct ihidev_attach_arg *iha = aux;
	struct wsmousedev_attach_args a;
	uint8_t mode = IMT_INPUT_MODE_PTP;
	void *desc;
	int size, repid;

	sc->sc_hdev.sc_idev = self;
	sc->sc_hdev.sc_intr = imt_intr;
	sc->sc_hdev.sc_parent = iha->parent;
	sc->sc_hdev.sc_report_id = iha->reportid;

	ihidev_get_report_desc(iha->parent, &desc, &size);
	repid = iha->reportid;
	sc->sc_hdev.sc_isize = hid_report_size(desc, size, hid_input, repid);
	sc->sc_hdev.sc_osize = hid_report_size(desc, size, hid_output, repid);
	sc->sc_hdev.sc_fsize = hid_report_size(desc, size, hid_feature, repid);

	if (!imt_parse_input(sc, desc, size, repid)) {
		aprint_error_dev(self, "could not parse touchpad report\n");
		return;
	}
	if (!imt_find_config(desc, size, &sc->sc_cfg_rid)) {
		aprint_error_dev(self, "no Input Mode report\n");
		return;
	}

	/*
	 * Switch the pad from mouse emulation to Precision Touchpad mode.
	 * Until this succeeds the multitouch report stays silent.
	 */
	if (ihidev_set_report((device_t)sc->sc_hdev.sc_parent, hid_feature,
	    sc->sc_cfg_rid, &mode, sizeof(mode))) {
		aprint_error_dev(self, "could not enable touchpad mode\n");
		return;
	}

	aprint_normal(": %d contacts%s, two-finger scrolling\n",
	    sc->sc_nslots, sc->sc_have_btn ? ", clickpad" : "");

	if (!pmf_device_register(self, NULL, NULL))
		aprint_error_dev(self, "couldn't establish power handler\n");

	imt_sysctl_setup();

	a.accessops = &imt_accessops;
	a.accesscookie = sc;
	sc->sc_wsmousedev = config_found(self, &a, wsmousedevprint, CFARGS_NONE);
}

static int
imt_detach(device_t self, int flags)
{
	struct imt_softc *sc = device_private(self);
	int error = 0;

	if (sc->sc_wsmousedev != NULL)
		error = config_detach(sc->sc_wsmousedev, flags);
	pmf_device_deregister(self);
	return error;
}

static void
imt_intr(struct ihidev *addr, void *buf, u_int len)
{
	struct imt_softc *sc = (struct imt_softc *)addr;
	uint8_t *data = buf;
	uint32_t btn = 0;
	int i, down = 0, x = 0, y = 0, dx, dy, s;

	if (!sc->sc_enabled || sc->sc_wsmousedev == NULL)
		return;
	if (len < (u_int)sc->sc_hdev.sc_isize)
		return;

	if (sc->sc_have_btn && hid_get_udata(data, &sc->sc_loc_btn))
		btn = 1;

	/*
	 * Take the first contact whose tip is down as the one that drives
	 * motion or scrolling, and count how many are down to choose which.
	 */
	for (i = 0; i < sc->sc_nslots; i++) {
		if (!sc->sc_contacts[i].valid)
			continue;
		if (!hid_get_udata(data, &sc->sc_contacts[i].loc_tip))
			continue;
		if (down == 0) {
			x = (int)hid_get_udata(data, &sc->sc_contacts[i].loc_x);
			y = (int)hid_get_udata(data, &sc->sc_contacts[i].loc_y);
		}
		down++;
	}

	dx = dy = 0;
	if (down > 0 && down == sc->sc_prev_down) {
		dx = x - sc->sc_prev_x;
		dy = y - sc->sc_prev_y;
		/* A lift-and-replace looks like a huge jump; ignore it. */
		if (dx > IMT_JUMP_LIMIT || dx < -IMT_JUMP_LIMIT ||
		    dy > IMT_JUMP_LIMIT || dy < -IMT_JUMP_LIMIT)
			dx = dy = 0;
	}

	s = spltty();
	if (down == 2) {
		/*
		 * Two fingers: scroll on the wheel axes.  Accumulate the
		 * remainder so a slow drag still scrolls instead of being
		 * truncated away on every report.
		 */
		int z = 0, w = 0;

		sc->sc_acc_y += dy;
		sc->sc_acc_x += dx;
		if (imt_scroll_div > 0) {
			z = sc->sc_acc_y / imt_scroll_div;
			w = sc->sc_acc_x / imt_scroll_div;
			sc->sc_acc_y -= z * imt_scroll_div;
			sc->sc_acc_x -= w * imt_scroll_div;
		}
		if (imt_scroll_invert) {
			z = -z;
			w = -w;
		}
		if (z != 0 || w != 0 || btn != sc->sc_prev_btn)
			wsmouse_input(sc->sc_wsmousedev, btn, 0, 0, z, w,
			    WSMOUSE_INPUT_DELTA);
	} else if (down == 1 || btn != sc->sc_prev_btn) {
		/* One finger (or a button change): ordinary pointer motion. */
		if (imt_motion_div > 0) {
			dx /= imt_motion_div;
			dy /= imt_motion_div;
		}
		if (down != 1)
			dx = dy = 0;
		/* wsmouse's Y grows upward; the pad's grows downward. */
		wsmouse_input(sc->sc_wsmousedev, btn, dx, -dy, 0, 0,
		    WSMOUSE_INPUT_DELTA);
	}
	splx(s);

	if (down != sc->sc_prev_down)
		sc->sc_acc_x = sc->sc_acc_y = 0;
	sc->sc_prev_x = x;
	sc->sc_prev_y = y;
	sc->sc_prev_down = down;
	sc->sc_prev_btn = btn;
}

static int
imt_enable(void *v)
{
	struct imt_softc *sc = v;
	int error;

	if (sc->sc_enabled)
		return EBUSY;
	if ((error = ihidev_open(&sc->sc_hdev)) != 0)
		return error;
	sc->sc_prev_down = 0;
	sc->sc_prev_btn = 0;
	sc->sc_enabled = true;
	return 0;
}

static void
imt_disable(void *v)
{
	struct imt_softc *sc = v;

	if (!sc->sc_enabled)
		return;
	sc->sc_enabled = false;
	ihidev_close(&sc->sc_hdev);
}

static int
imt_ioctl(void *v, u_long cmd, void *data, int flag, struct lwp *l)
{

	switch (cmd) {
	case WSMOUSEIO_GTYPE:
		*(u_int *)data = WSMOUSE_TYPE_TPANEL;
		return 0;
	default:
		return EPASSTHROUGH;
	}
}
