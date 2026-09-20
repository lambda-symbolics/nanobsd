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
#include <sys/pmf.h>

#include <dev/i2c/i2cvar.h>
#include <dev/i2c/ihidev.h>

#include <dev/hid/hid.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#define IMT_MAX_CONTACTS	10
#define IMT_INPUT_MODE_MOUSE	0x00
#define IMT_INPUT_MODE_PTP	0x03

/*
 * Windows Precision Touchpad configuration usages.  Digitizers page unless
 * noted; the certification blob lives on a vendor page and Linux reads it
 * purely for the side effect of unlocking reporting on some devices.
 */
#define IMT_USAGE_SURFACE_SW	HID_USAGE2(HUP_DIGITIZERS, 0x57)
#define IMT_USAGE_BUTTON_SW	HID_USAGE2(HUP_DIGITIZERS, 0x58)
#define IMT_USAGE_LATENCY	HID_USAGE2(HUP_DIGITIZERS, 0x60)
#define IMT_USAGE_CONTACTMAX	HID_USAGE2(HUP_DIGITIZERS, 0x55)
#define IMT_USAGE_SCANTIME	HID_USAGE2(HUP_DIGITIZERS, 0x56)
#define IMT_USAGE_CERT		0xff0000c5

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
	int			sc_cfg_size;	/* ...and its length in bytes */

	/* gesture state, carried between reports */
	int			sc_prev_x, sc_prev_y;
	int			sc_prev_down;
	uint32_t		sc_prev_btn;
	/* sub-click scroll remainder, so slow drags still scroll */
	int			sc_acc_x, sc_acc_y;
	int			sc_traced;

	/* descriptor, kept for the descriptor-driven PTP handshake */
	void			*sc_desc;
	int			sc_dlen;

	int			sc_cert_rid;	/* Win8 certification blob */
	int			sc_contactmax_rid;
	int			sc_surface_rid;
	struct hid_location	sc_loc_surface;
	int			sc_button_rid;
	struct hid_location	sc_loc_button;
	int			sc_latency_rid;
	struct hid_location	sc_loc_latency;

	uint32_t		sc_prev_ids;	/* which contact IDs were down */

	struct hid_location	sc_loc_scantime;
	bool			sc_have_scantime;

	/*
	 * A frame under construction.  This pad reports in hybrid mode: one
	 * contact per packet, with the contact count carried only in the
	 * first packet of a frame and zero in the continuations.
	 */
	struct imt_fcontact {
		int	id, x, y, tip;
	}			sc_frame[IMT_MAX_CONTACTS];
	int			sc_frame_n;	/* records collected */
	int			sc_frame_expect;/* records the frame will have */
	uint32_t		sc_scantime;
};

static int	imt_match(device_t, cfdata_t, void *);
static void	imt_attach(device_t, device_t, void *);
static int	imt_detach(device_t, int);
static void	imt_intr(struct ihidev *, void *, u_int);
static void	imt_emit(struct imt_softc *, uint32_t);

static int	imt_set_mode(struct imt_softc *, uint8_t);
static void	imt_scan_features(struct imt_softc *, const void *, int);
static int	imt_ptp_init(struct imt_softc *);
static bool	imt_resume(device_t, const pmf_qual_t *);
static int	imt_enable(void *);
static void	imt_disable(void *);
static int	imt_ioctl(void *, u_long, void *, int, struct lwp *);

int imt_debug = 0;
/*
 * Switching the pad into Precision Touchpad mode makes it stop sending the
 * mouse report -- and on this Elan pad it then sends NOTHING AT ALL, not even
 * the multitouch report, so the pointer dies completely.  Until that is
 * understood the switch is opt-in: left off, the pad keeps working normally
 * through ims(4) and imt just sits idle.
 *	sysctl -w machdep.imt.ptp=1	(then replug/restart X to re-open)
 */
int imt_ptp = 1;	/* pushing on it: leave PTP mode enabled */
int imt_motion_div = IMT_MOTION_DIV_DEFAULT;
int imt_scroll_div = IMT_SCROLL_DIV_DEFAULT;
int imt_scroll_invert = 0;

static struct sysctllog *imt_sysctllog;
static struct imt_softc *imt_instance;	/* for the live mode toggle */


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
		/* Padding carries stale usage labels; see imt_scan_features. */
		if ((h.flags & HIO_CONST) != 0)
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
		case IMT_USAGE_SCANTIME:
			sc->sc_loc_scantime = h.loc;
			sc->sc_have_scantime = true;
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
 * Writing machdep.imt.ptp switches the pad NOW rather than at the next open.
 * The pad keeps its mode across a warm reboot, so without this a pad left in
 * Precision Touchpad mode stays silent no matter what the driver does next,
 * and the only way back is a full power cycle.
 */
static int
imt_sysctl_ptp(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int error, val;

	val = imt_ptp;
	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error != 0 || newp == NULL)
		return error;
	if (val != 0 && val != 1)
		return EINVAL;
	imt_ptp = val;
	/*
	 * Re-run the whole handshake, not just the mode write: the point of
	 * the toggle is to compare the two bring-ups.
	 */
	if (imt_instance != NULL)
		(void)imt_ptp_init(imt_instance);
	return 0;
}

static int
imt_sysctl_kick(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int val = 0, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (imt_instance == NULL)
		return ENXIO;

	(void)ihidev_kick((device_t)imt_instance->sc_hdev.sc_parent);
	(void)imt_ptp_init(imt_instance);
	return 0;
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
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "ptp",
	    SYSCTL_DESCR("1 = Precision Touchpad mode, 0 = mouse mode; applied at once"),
	    imt_sysctl_ptp, 0, &imt_ptp, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "scroll_invert",
	    SYSCTL_DESCR("reverse the two-finger scroll direction"),
	    NULL, 0, &imt_scroll_invert, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "kick",
	    SYSCTL_DESCR("power on, reset and re-run the PTP handshake"),
	    imt_sysctl_kick, 0, NULL, 0, CTL_CREATE, CTL_EOL);
	sysctl_createv(&imt_sysctllog, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE, CTLTYPE_INT, "debug",
	    SYSCTL_DESCR("1 = log emitted scrolls, 2 = log every report"),
	    NULL, 0, &imt_debug, 0, CTL_CREATE, CTL_EOL);
}

/*
 * hid(9) can read a field out of a report but not write one, and the
 * configuration reports pack several unrelated switches into shared bytes, so
 * they have to be modified in place rather than rebuilt from zeroes.  Bit
 * order matches hid_get_udata(): little-endian from loc->pos.
 */
static void
imt_put_udata(uint8_t *buf, const struct hid_location *loc, u_long val)
{
	u_int i, bit;

	if (loc->size == 0 || loc->size > 32)
		return;
	for (i = 0; i < loc->size; i++) {
		bit = loc->pos + i;
		if (val & (1UL << i))
			buf[bit / 8] |= 1 << (bit % 8);
		else
			buf[bit / 8] &= ~(1 << (bit % 8));
	}
}

/*
 * Read-modify-write fields of a feature report, verifying the result.
 *
 * Reading first matters: zeroing the bytes we do not own would clear whatever
 * else shares the report, and Surface and Button Switch share one.  A failed
 * read is therefore fatal rather than an excuse to write zeroes - doing that
 * would clear the field written by the previous call.  Both fields are set in
 * one read-modify-write for the same reason.
 */
static int
imt_set_fields(struct imt_softc *sc, int rid,
    const struct hid_location *loc1, u_long val1,
    const struct hid_location *loc2, u_long val2, bool complete,
    const char *what)
{
	device_t parent = (device_t)sc->sc_hdev.sc_parent;
	uint8_t rep[32];
	int len, err = 0;

	len = hid_report_size(sc->sc_desc, sc->sc_dlen, hid_feature, rid);
	if (len <= 0 || len > (int)sizeof(rep)) {
		printf("imt: %s: rid %d has implausible length %d\n", what,
		    rid, len);
		return EINVAL;
	}

	memset(rep, 0, sizeof(rep));
	if (ihidev_get_report(parent, hid_feature, rid, rep, len) != 0) {
		/*
		 * This pad refuses GET_REPORT on its configuration reports.
		 * Starting from zeroes is only safe when this one write
		 * covers every writable field of the report, which is why
		 * both switches are set together; otherwise skip the write
		 * rather than clear a field belonging to someone else.
		 */
		if (!complete) {
			if (imt_debug)
				printf("imt: %s: rid %d unreadable, "
				    "skipping\n", what, rid);
			return 0;
		}
		if (imt_debug)
			printf("imt: %s: rid %d unreadable, writing from "
			    "zeroes\n", what, rid);
		memset(rep, 0, sizeof(rep));
	}
	if (loc1 != NULL)
		imt_put_udata(rep, loc1, val1);
	if (loc2 != NULL)
		imt_put_udata(rep, loc2, val2);
	err = ihidev_set_report(parent, hid_feature, rid, rep, len);

	/*
	 * Report what the field actually became where the device allows it to
	 * be read, and say so plainly when it does not, rather than echoing
	 * the value we asked for and calling that confirmation.
	 */
	{
		uint8_t back[32];

		if (err != 0)
			printf("imt: %s: writing rid %d failed (%d)\n", what,
			    rid, err);
		if (!imt_debug)
			return err;

		memset(back, 0, sizeof(back));
		if (ihidev_get_report(parent, hid_feature, rid, back, len) == 0)
			printf("imt: %s rid %d -> err %d, now %lu/%lu "
			    "(raw %02x %02x)\n", what, rid, err,
			    loc1 != NULL ? hid_get_udata(back, loc1) : 0,
			    loc2 != NULL ? hid_get_udata(back, loc2) : 0,
			    back[0], back[1]);
		else
			printf("imt: %s rid %d -> err %d, wrote %02x %02x "
			    "(unverifiable, not readable)\n", what, rid, err,
			    rep[0], rep[1]);
	}
	return err;
}

/*
 * Log every feature field the pad advertises and remember the ones the
 * Precision Touchpad handshake needs.  The logging is the point as much as
 * the lookup: which of these reports the device actually implements is the
 * open question, and guessing report IDs is what went wrong before.
 */
static void
imt_scan_features(struct imt_softc *sc, const void *desc, int dlen)
{
	struct hid_data *hd;
	struct hid_item h;
	int last_rid = -1;
	uint32_t last_usage = 0xffffffff;

	sc->sc_cert_rid = -1;
	sc->sc_contactmax_rid = -1;
	sc->sc_surface_rid = -1;
	sc->sc_button_rid = -1;
	sc->sc_latency_rid = -1;

	hd = hid_start_parse(desc, dlen, hid_feature);
	if (hd == NULL)
		return;
	while (hid_get_item(hd, &h)) {
		/*
		 * Skip constant (padding) items.  NetBSD's parser keeps the
		 * usage array after an item ends, so constant padding that
		 * declares no usage of its own comes back labelled with the
		 * *previous* field's usage.  Report 5 on this pad really does
		 * look like
		 *
		 *   usage 0x000d0057 pos 0   <- Surface Switch
		 *   usage 0x000d0058 pos 1   <- Button Switch
		 *   usage 0x000d0057 pos 2   <- padding, stale label
		 *   usage 0x000d0058 pos 3   <- padding, stale label
		 *
		 * so a scanner that takes the last match writes bits 2 and 3
		 * and leaves the two real enable bits clear.  Those switches
		 * gate which inputs raise an interrupt, which is exactly why
		 * the pad worked in mouse mode and went silent in Precision
		 * Touchpad mode.
		 */
		if (h.kind != hid_feature || (h.flags & HIO_CONST) != 0)
			continue;
		/*
		 * The certification blob is hundreds of single-byte fields;
		 * log one line per (report, usage) run instead of per byte.
		 */
		if (h.report_ID != last_rid || h.usage != last_usage) {
				if (imt_debug > 1)
				printf("imt: feature rid %d usage 0x%08x "
				    "pos %u size %u flags 0x%x\n",
				    h.report_ID, h.usage, h.loc.pos,
				    h.loc.size, h.flags);
			last_rid = h.report_ID;
			last_usage = h.usage;
		}
		switch (h.usage) {
		case IMT_USAGE_CERT:
			/*
			 * Only the report that *opens* with the blob usage is
			 * the certification report; the usage also appears
			 * mid-report elsewhere.  This is the same test Linux
			 * makes with usage_index == 0, and taking the first
			 * match in parse order instead picked a four-byte
			 * report that returned different garbage on each read.
			 */
			if (h.loc.pos == 0)
				sc->sc_cert_rid = h.report_ID;
			break;
		case IMT_USAGE_CONTACTMAX:
			sc->sc_contactmax_rid = h.report_ID;
			break;
		case IMT_USAGE_SURFACE_SW:
			if (sc->sc_surface_rid < 0) {
				sc->sc_surface_rid = h.report_ID;
				sc->sc_loc_surface = h.loc;
			}
			break;
		case IMT_USAGE_BUTTON_SW:
			if (sc->sc_button_rid < 0) {
				sc->sc_button_rid = h.report_ID;
				sc->sc_loc_button = h.loc;
			}
			break;
		case IMT_USAGE_LATENCY:
			if (sc->sc_latency_rid < 0) {
				sc->sc_latency_rid = h.report_ID;
				sc->sc_loc_latency = h.loc;
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);
}

/*
 * The full Precision Touchpad bring-up, in the order Windows performs it.
 * Setting Input Mode alone left this pad in a state where it acknowledged
 * the mode and then reported nothing at all, so the steps around it are the
 * candidates for whatever it is waiting on.
 */
static int
imt_ptp_init(struct imt_softc *sc)
{
	device_t parent = (device_t)sc->sc_hdev.sc_parent;
	uint8_t blob[512];
	int len, err = 0;

	/*
	 * Read the vendor certification blob.  Linux does this with the
	 * comment "retrieve the Win8 blob once to enable some devices" and
	 * discards the contents; the read itself is the operation.
	 */
	if (sc->sc_cert_rid >= 0) {
		len = hid_report_size(sc->sc_desc, sc->sc_dlen, hid_feature,
		    sc->sc_cert_rid);
		if (len > (int)sizeof(blob))
			len = (int)sizeof(blob);
		if (len > 0) {
			memset(blob, 0, sizeof(blob));
			err = ihidev_get_report(parent, hid_feature,
			    sc->sc_cert_rid, blob, len);
			if (imt_debug)
			printf("imt: cert blob rid %d len %d -> err %d, "
			    "%02x %02x %02x %02x\n", sc->sc_cert_rid, len,
			    err, blob[0], blob[1], blob[2], blob[3]);
		}
	} else
		printf("imt: no certification report in descriptor\n");

	/*
	 * Linux reads Contact Max as a feature too.  Its value is not used
	 * here (the slot count comes from the input report), but the read is
	 * part of the sequence a Windows host performs.
	 */
	if (sc->sc_contactmax_rid >= 0) {
		len = hid_report_size(sc->sc_desc, sc->sc_dlen, hid_feature,
		    sc->sc_contactmax_rid);
		if (len > 0 && len <= (int)sizeof(blob)) {
			memset(blob, 0, sizeof(blob));
			err = ihidev_get_report(parent, hid_feature,
			    sc->sc_contactmax_rid, blob, len);
			if (imt_debug)
				printf("imt: contact max rid %d -> err %d, %02x %02x\n",
			    sc->sc_contactmax_rid, err, blob[0], blob[1]);
		}
	}

	/*
	 * Surface and button reporting.  These gate which inputs raise an
	 * interrupt, so getting them wrong looks exactly like a dead pad.
	 */
	if (sc->sc_surface_rid >= 0 &&
	    sc->sc_surface_rid == sc->sc_button_rid)
		(void)imt_set_fields(sc, sc->sc_surface_rid,
		    &sc->sc_loc_surface, 1, &sc->sc_loc_button, 1, true,
		    "surface+button switch");
	else {
		if (sc->sc_surface_rid >= 0)
			(void)imt_set_fields(sc, sc->sc_surface_rid,
			    &sc->sc_loc_surface, 1, NULL, 0, false,
			    "surface switch");
		if (sc->sc_button_rid >= 0)
			(void)imt_set_fields(sc, sc->sc_button_rid,
			    &sc->sc_loc_button, 1, NULL, 0, false,
			    "button switch");
	}
	/*
	 * Setting Input Mode is the step that matters most, so press on even
	 * if the switches could not be written.
	 */

	err = imt_set_mode(sc, imt_ptp ? IMT_INPUT_MODE_PTP :
	    IMT_INPUT_MODE_MOUSE);

	/* Normal latency, not the high-latency power saving mode. */
	/*
	 * Normal latency is the power-on default, and report 7 also carries
	 * Button Type, which is not ours to overwrite, so this is skipped
	 * rather than forced when the report cannot be read back.
	 */
	if (sc->sc_latency_rid >= 0)
		(void)imt_set_fields(sc, sc->sc_latency_rid,
		    &sc->sc_loc_latency, 0, NULL, 0, false, "latency mode");

	return err;
}

/*
 * ihidev resets the device on resume, and a host-initiated reset discards
 * the selected input mode, so the handshake has to be redone.
 */
static bool
imt_resume(device_t self, const pmf_qual_t *qual)
{
	struct imt_softc *sc = device_private(self);

	if (sc->sc_enabled)
		(void)imt_ptp_init(sc);
	return true;
}

/*
 * Put the pad into Precision Touchpad mode.  This must be redone every time
 * the device is opened, not only at attach: the pad is reset when ihidev
 * brings it up, and a mode set before that is forgotten, leaving it in
 * mouse-emulation mode where the multitouch report is never sent at all.
 */
static int
imt_set_mode(struct imt_softc *sc, uint8_t want)
{
	uint8_t rep[8];
	int len, err = 0;

	/*
	 * Send the whole feature report, not a single byte.  The report is
	 * "input mode" plus a device index, and a short write is accepted
	 * without complaint while leaving the pad in mouse-emulation mode,
	 * so the multitouch report never arrives.
	 */
	len = sc->sc_cfg_size;
	if (len <= 0 || len > (int)sizeof(rep))
		len = 2;
	memset(rep, 0, sizeof(rep));
	rep[0] = want;

	err = ihidev_set_report((device_t)sc->sc_hdev.sc_parent, hid_feature,
	    sc->sc_cfg_rid, rep, len);
	if (imt_debug)
		printf("imt: set input mode rid %d len %d -> %d (err %d)\n",
	    sc->sc_cfg_rid, len, rep[0], err);

	/*
	 * Read the mode back.  A SET_REPORT that the pad NAKs internally
	 * still returns success here, so the write returning 0 is not
	 * evidence that the pad actually changed mode.
	 */
	if (imt_debug) {
		memset(rep, 0, sizeof(rep));
		if (ihidev_get_report((device_t)sc->sc_hdev.sc_parent,
		    hid_feature, sc->sc_cfg_rid, rep, len) == 0)
			printf("imt: input mode reads back %d %d\n", rep[0],
			    rep[1]);
		else
			printf("imt: input mode readback failed\n");
	}

	return err;
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
		if (imt_debug > 1)
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
	void *desc;
	int size, repid;

	sc->sc_hdev.sc_idev = self;
	sc->sc_hdev.sc_intr = imt_intr;
	sc->sc_hdev.sc_parent = iha->parent;
	sc->sc_hdev.sc_report_id = iha->reportid;

	ihidev_get_report_desc(iha->parent, &desc, &size);
	sc->sc_desc = desc;
	sc->sc_dlen = size;
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
	sc->sc_cfg_size = hid_report_size(desc, size, hid_feature,
	    sc->sc_cfg_rid);
	imt_scan_features(sc, desc, size);
	if (imt_debug)
		printf("imt: input report %d size %d, config report %d size %d\n",
		    repid, sc->sc_hdev.sc_isize, sc->sc_cfg_rid,
		    sc->sc_cfg_size);

	aprint_normal(": %d contacts%s, two-finger scrolling\n",
	    sc->sc_nslots, sc->sc_have_btn ? ", clickpad" : "");

	if (!pmf_device_register(self, NULL, imt_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");

	imt_instance = sc;
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
imt_emit(struct imt_softc *sc, uint32_t btn)
{
	int i, down = 0, x = 0, y = 0, dx, dy, s, sumx = 0, sumy = 0;
	uint32_t ids = 0;

	/*
	 * Contact Count covers the whole frame regardless of tip state, so a
	 * record with the tip up is still part of it; it just is not a finger.
	 */
	for (i = 0; i < sc->sc_frame_n; i++) {
		if (!sc->sc_frame[i].tip)
			continue;
		sumx += sc->sc_frame[i].x;
		sumy += sc->sc_frame[i].y;
		ids |= 1U << (sc->sc_frame[i].id & 31);
		down++;
	}
	if (down > 0) {
		x = sumx / down;
		y = sumy / down;
	}

	/*
	 * A delta only means anything between the same fingers.  An equal
	 * finger count is not enough: one finger can lift while another lands
	 * in the same frame, and the centroid then jumps.
	 */
	dx = dy = 0;
	if (down > 0 && down == sc->sc_prev_down && ids == sc->sc_prev_ids) {
		dx = x - sc->sc_prev_x;
		dy = y - sc->sc_prev_y;
		if (dx > IMT_JUMP_LIMIT || dx < -IMT_JUMP_LIMIT ||
		    dy > IMT_JUMP_LIMIT || dy < -IMT_JUMP_LIMIT)
			dx = dy = 0;
	}

	if (imt_debug > 1 && sc->sc_traced < 60) {
		sc->sc_traced++;
		printf("imt: frame n=%d down=%d x=%d y=%d dx=%d dy=%d btn=%u\n",
		    sc->sc_frame_n, down, x, y, dx, dy, btn);
	}

	s = spltty();
	if (down == 2) {
		/*
		 * Two fingers: scroll on the wheel axes.  Accumulate the
		 * remainder so a slow drag still scrolls instead of being
		 * truncated away on every frame.
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
		if (z != 0 || w != 0 || btn != sc->sc_prev_btn) {
			if (imt_debug)
				printf("imt: scroll z=%d w=%d\n", z, w);
			wsmouse_input(sc->sc_wsmousedev, btn, 0, 0, z, w,
			    WSMOUSE_INPUT_DELTA);
		}
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

	if (down != sc->sc_prev_down || ids != sc->sc_prev_ids)
		sc->sc_acc_x = sc->sc_acc_y = 0;
	sc->sc_prev_ids = ids;
	sc->sc_prev_x = x;
	sc->sc_prev_y = y;
	sc->sc_prev_down = down;
	sc->sc_prev_btn = btn;
}

/*
 * Assemble a contact frame from one or more packets before acting on it.
 *
 * In hybrid reporting a packet carries as many contact records as fit in the
 * report - one, here - and the Contact Count appears only in the first packet
 * of a frame, with zero standing for "continuation".  Treating each packet as
 * a finished frame therefore sees two separate one-finger frames during a
 * two-finger gesture, and the scroll branch can never be reached no matter
 * how the sensitivity is tuned.
 */
static void
imt_intr(struct ihidev *addr, void *buf, u_int len)
{
	struct imt_softc *sc = (struct imt_softc *)addr;
	uint8_t *data = buf;
	struct imt_fcontact *f;
	uint32_t btn = 0;
	int i, cnt = 0;

	if (!sc->sc_enabled || sc->sc_wsmousedev == NULL)
		return;
	if (len < (u_int)sc->sc_hdev.sc_isize)
		return;

	if (sc->sc_have_btn && hid_get_udata(data, &sc->sc_loc_btn))
		btn = 1;
	if (sc->sc_loc_count.size > 0)
		cnt = (int)hid_get_udata(data, &sc->sc_loc_count);
	if (sc->sc_have_scantime)
		sc->sc_scantime = (uint32_t)hid_get_udata(data,
		    &sc->sc_loc_scantime);

	if (imt_debug > 1 && sc->sc_traced < 60) {
		sc->sc_traced++;
		printf("imt: pkt cnt=%d scan=%u btn=%u slot0 tip=%lu id=%lu "
		    "x=%lu y=%lu\n", cnt, sc->sc_scantime, btn,
		    sc->sc_nslots > 0 ?
		    hid_get_udata(data, &sc->sc_contacts[0].loc_tip) : 0,
		    sc->sc_nslots > 0 ?
		    hid_get_udata(data, &sc->sc_contacts[0].loc_id) : 0,
		    sc->sc_nslots > 0 ?
		    hid_get_udata(data, &sc->sc_contacts[0].loc_x) : 0,
		    sc->sc_nslots > 0 ?
		    hid_get_udata(data, &sc->sc_contacts[0].loc_y) : 0);
	}

	if (cnt > 0) {
		/* First packet of a frame. */
		sc->sc_frame_expect = cnt;
		if (sc->sc_frame_expect > IMT_MAX_CONTACTS)
			sc->sc_frame_expect = IMT_MAX_CONTACTS;
		sc->sc_frame_n = 0;
	} else if (sc->sc_frame_expect == 0) {
		/* Continuation with no frame open; nothing to attach it to. */
		return;
	}

	for (i = 0; i < sc->sc_nslots; i++) {
		if (!sc->sc_contacts[i].valid)
			continue;
		if (sc->sc_frame_n >= sc->sc_frame_expect)
			break;
		f = &sc->sc_frame[sc->sc_frame_n++];
		f->tip = hid_get_udata(data, &sc->sc_contacts[i].loc_tip) ?
		    1 : 0;
		f->id = sc->sc_contacts[i].loc_id.size > 0 ?
		    (int)hid_get_udata(data, &sc->sc_contacts[i].loc_id) : 0;
		f->x = (int)hid_get_udata(data, &sc->sc_contacts[i].loc_x);
		f->y = (int)hid_get_udata(data, &sc->sc_contacts[i].loc_y);
	}

	if (sc->sc_frame_n < sc->sc_frame_expect)
		return;		/* wait for the rest of the frame */

	imt_emit(sc, btn);
	sc->sc_frame_n = 0;
	sc->sc_frame_expect = 0;
}

static int
imt_enable(void *v)
{
	struct imt_softc *sc = v;
	int error;

	if (sc->sc_enabled)
		return EBUSY;
	if ((error = ihidev_open(&sc->sc_hdev)) != 0) {
		if (imt_debug)
			printf("imt: ihidev_open failed %d\n", error);
		return error;
	}
	/*
	 * A pad that cannot be put into Precision Touchpad mode is of no use
	 * to this driver, and ims(4) still provides mouse mode, so report the
	 * failure rather than pretending to have opened successfully.
	 */
	if ((error = imt_ptp_init(sc)) != 0) {
		printf("imt: PTP initialisation failed (%d)\n", error);
		ihidev_close(&sc->sc_hdev);
		return error;
	}
	sc->sc_prev_down = 0;
	sc->sc_prev_btn = 0;
	sc->sc_prev_ids = 0;
	sc->sc_frame_n = 0;
	sc->sc_frame_expect = 0;
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
