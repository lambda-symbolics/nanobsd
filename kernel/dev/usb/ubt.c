/*	$NetBSD: ubt.c,v 1.67 2024/07/05 04:31:52 rin Exp $	*/

/*-
 * Copyright (c) 2006 Itronix Inc.
 * All rights reserved.
 *
 * Written by Iain Hibbert for Itronix Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of Itronix Inc. may not be used to endorse
 *    or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ITRONIX INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL ITRONIX INC. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) 2002, 2003 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) and
 * David Sainty (dsainty@NetBSD.org).
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
/*
 * This driver originally written by Lennart Augustsson and David Sainty,
 * but was mostly rewritten for the NetBSD Bluetooth protocol stack by
 * Iain Hibbert for Itronix, Inc using the FreeBSD ng_ubt.c driver as a
 * reference.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: ubt.c,v 1.67 2024/07/05 04:31:52 rin Exp $");

#ifdef _KERNEL_OPT
#include "opt_usb.h"
#endif

#include <sys/param.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdevs.h>
#include <dev/firmload.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/time.h>

#include <netbt/bluetooth.h>
#include <netbt/hci.h>

/*******************************************************************************
 *
 *	debugging stuff
 */
#undef DPRINTF
#undef DPRINTFN

#ifdef UBT_DEBUG
int	ubt_debug = 0;

#define DPRINTF(...)		do {		\
	if (ubt_debug) {			\
		printf("%s: ", __func__);	\
		printf(__VA_ARGS__);		\
	}					\
} while (/* CONSTCOND */0)

#define DPRINTFN(n, ...)	do {		\
	if (ubt_debug > (n)) {			\
		printf("%s: ", __func__);	\
		printf(__VA_ARGS__);		\
	}					\
} while (/* CONSTCOND */0)

SYSCTL_SETUP(sysctl_hw_ubt_debug_setup, "sysctl hw.ubt_debug setup")
{

	sysctl_createv(NULL, 0, NULL, NULL,
		CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
		CTLTYPE_INT, "ubt_debug",
		SYSCTL_DESCR("ubt debug level"),
		NULL, 0,
		&ubt_debug, sizeof(ubt_debug),
		CTL_HW, CTL_CREATE, CTL_EOL);
}
#else
#define DPRINTF(...)
#define DPRINTFN(...)
#endif

/*******************************************************************************
 *
 *	ubt softc structure
 *
 */

/* buffer sizes */
/*
 * NB: although ACL packets can extend to 65535 bytes, most devices
 * have max_acl_size at much less (largest I have seen is 384)
 */
#define UBT_BUFSIZ_CMD		(HCI_CMD_PKT_SIZE - 1)
#define UBT_BUFSIZ_ACL		(2048 - 1)
#define UBT_BUFSIZ_EVENT	(HCI_EVENT_PKT_SIZE - 1)

/* Transmit timeouts */
#define UBT_CMD_TIMEOUT		USBD_DEFAULT_TIMEOUT

/* LISPBSD: Intel controllers get their firmware loaded at enable */
#define UBT_INTEL_VENDOR	0x8087
#define UBT_ACL_TIMEOUT		USBD_DEFAULT_TIMEOUT

/*
 * ISOC transfers
 *
 * xfer buffer size depends on the frame size, and the number
 * of frames per transfer is fixed, as each frame should be
 * 1ms worth of data. This keeps the rate that xfers complete
 * fairly constant. We use multiple xfers to keep the hardware
 * busy
 */
#define UBT_NXFERS		3	/* max xfers to queue */
#define UBT_NFRAMES		10	/* frames per xfer */

struct ubt_isoc_xfer {
	struct ubt_softc	*softc;
	struct usbd_xfer	*xfer;
	uint8_t			*buf;
	uint16_t		 size[UBT_NFRAMES];
	int			 busy;
};

struct ubt_intel;

struct ubt_softc {
	device_t		 sc_dev;
	struct usbd_device	*sc_udev;
	int			 sc_refcnt;
	int			 sc_dying;
	int			 sc_enabled;
	int			 sc_parked;	/* LISPBSD: inbound pipes aborted for a USB suspend */

	/* Control Interface */
	struct usbd_interface *	 sc_iface0;

	/* Commands (control) */
	struct usbd_xfer	*sc_cmd_xfer;
	uint8_t			*sc_cmd_buf;
	int			 sc_cmd_busy;	/* write active */
	MBUFQ_HEAD()		 sc_cmd_queue;	/* output queue */

	/* Events (interrupt) */
	int			 sc_evt_addr;	/* endpoint address */
	struct usbd_pipe	*sc_evt_pipe;
	uint8_t			*sc_evt_buf;

	/* ACL data (in) */
	int			 sc_aclrd_addr;	/* endpoint address */
	struct usbd_pipe	*sc_aclrd_pipe;	/* read pipe */
	struct usbd_xfer	*sc_aclrd_xfer;	/* read xfer */
	uint8_t			*sc_aclrd_buf;	/* read buffer */
	int			 sc_aclrd_busy;	/* reading */

	/* ACL data (out) */
	int			 sc_aclwr_addr;	/* endpoint address */
	struct usbd_pipe	*sc_aclwr_pipe;	/* write pipe */
	struct usbd_xfer	*sc_aclwr_xfer;	/* write xfer */
	uint8_t			*sc_aclwr_buf;	/* write buffer */
	int			 sc_aclwr_busy;	/* write active */
	MBUFQ_HEAD()		 sc_aclwr_queue;/* output queue */

	/* ISOC interface */
	struct usbd_interface	*sc_iface1;	/* ISOC interface */
	struct sysctllog	*sc_log;	/* sysctl log */
	int			 sc_config;	/* current config no */
	int			 sc_alt_config;	/* no of alternates */

	/* SCO data (in) */
	int			 sc_scord_addr;	/* endpoint address */
	struct usbd_pipe	*sc_scord_pipe;	/* read pipe */
	int			 sc_scord_size;	/* frame length */
	struct ubt_isoc_xfer	 sc_scord[UBT_NXFERS];
	struct mbuf		*sc_scord_mbuf;	/* current packet */

	/* SCO data (out) */
	int			 sc_scowr_addr;	/* endpoint address */
	struct usbd_pipe	*sc_scowr_pipe;	/* write pipe */
	int			 sc_scowr_size;	/* frame length */
	struct ubt_isoc_xfer	 sc_scowr[UBT_NXFERS];
	struct mbuf		*sc_scowr_mbuf;	/* current packet */
	int			 sc_scowr_busy;	/* write active */
	MBUFQ_HEAD()		 sc_scowr_queue;/* output queue */

	/* Protocol structure */
	struct hci_unit		*sc_unit;
	struct bt_stats		 sc_stats;

	/* Successfully attached */
	int			 sc_ok;

	/* LISPBSD: Intel controllers need their firmware loaded (ubt_intel_fwload) */
	int			 sc_intel;
	int			 sc_intel_fwdone;
	int			 sc_intel_loading;	/* events go to the loader */
	struct ubt_intel	*sc_intel_ui;
};

static void ubt_intel_enqueue(struct ubt_intel *, const uint8_t *, uint32_t);

/*******************************************************************************
 *
 * Bluetooth unit/USB callback routines
 *
 */
static int ubt_enable(device_t);
static void ubt_disable(device_t);

static void ubt_xmit_cmd(device_t, struct mbuf *);
static void ubt_xmit_cmd_start(struct ubt_softc *);
static void ubt_xmit_cmd_complete(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_xmit_acl(device_t, struct mbuf *);
static void ubt_xmit_acl_start(struct ubt_softc *);
static void ubt_xmit_acl_complete(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_xmit_sco(device_t, struct mbuf *);
static void ubt_xmit_sco_start(struct ubt_softc *);
static void ubt_xmit_sco_start1(struct ubt_softc *, struct ubt_isoc_xfer *);
static void ubt_xmit_sco_complete(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_recv_event(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_recv_acl_start(struct ubt_softc *);
static void ubt_recv_acl_complete(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_recv_sco_start1(struct ubt_softc *, struct ubt_isoc_xfer *);
static void ubt_recv_sco_complete(struct usbd_xfer *,
				void *, usbd_status);

static void ubt_stats(device_t, struct bt_stats *, int);

static const struct hci_if ubt_hci = {
	.enable = ubt_enable,
	.disable = ubt_disable,
	.output_cmd = ubt_xmit_cmd,
	.output_acl = ubt_xmit_acl,
	.output_sco = ubt_xmit_sco,
	.get_stats = ubt_stats,
	.ipl = IPL_SOFTUSB,
};

/*******************************************************************************
 *
 * USB Autoconfig stuff
 *
 */

static int	ubt_match(device_t, cfdata_t, void *);
static void	ubt_attach(device_t, device_t, void *);
static int	ubt_detach(device_t, int);
static int	ubt_activate(device_t, enum devact);
static bool	ubt_suspend(device_t, const pmf_qual_t *);
static bool	ubt_resume(device_t, const pmf_qual_t *);

CFATTACH_DECL_NEW(ubt, sizeof(struct ubt_softc), ubt_match, ubt_attach,
    ubt_detach, ubt_activate);

static int ubt_set_isoc_config(struct ubt_softc *);
static int ubt_sysctl_config(SYSCTLFN_PROTO);
static void ubt_abortdealloc(struct ubt_softc *);

/*
 * To match or ignore, add details to the ubt_dev list.
 * Use value of -1 to indicate a wildcard
 * To override another entry, add details earlier
 */
static const struct ubt_devno {
	int			vendor;
	int			product;
	int			class;
	int			subclass;
	int			proto;
	int			match;
} ubt_dev[] = {
	{   /* ignore Broadcom 2033 without firmware */
	    USB_VENDOR_BROADCOM,
	    USB_PRODUCT_BROADCOM_BCM2033NF,
	    -1,
	    -1,
	    -1,
	    UMATCH_NONE
	},
	{   /* Apple Bluetooth Host Controller MacbookPro 7,1 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_1,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller iMac 11,1 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_2,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacBookPro 8,2 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_3,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacBookAir 3,1 3,2*/
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_4,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacBookAir 4,1 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_5,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacMini 5,1 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_6,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacBookAir 6,1 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_7,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Apple Bluetooth Host Controller MacBookPro 9,2 */
	    USB_VENDOR_APPLE,
	    USB_PRODUCT_APPLE_BLUETOOTH_HOST_8,
	    -1,
	    -1,
	    -1,
	    UMATCH_VENDOR_PRODUCT
	},
	{   /* Broadcom chips with PatchRAM support */
	    USB_VENDOR_BROADCOM,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Broadcom based device with PatchRAM support */
	    USB_VENDOR_FOXCONN,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Broadcom based device with PatchRAM support */
	    USB_VENDOR_LITEON,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Broadcom based device with PatchRAM support */
	    USB_VENDOR_BELKIN,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Broadcom based device with PatchRAM support */
	    USB_VENDOR_TOSHIBA,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Broadcom based device with PatchRAM support */
	    USB_VENDOR_ASUSTEK,
	    -1,
	    UDCLASS_VENDOR,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_VENDOR_DEVCLASS_DEVPROTO
	},
	{   /* Generic Bluetooth SIG compliant devices */
	    -1,
	    -1,
	    UDCLASS_WIRELESS,
	    UDSUBCLASS_RF,
	    UDPROTO_BLUETOOTH,
	    UMATCH_DEVCLASS_DEVSUBCLASS_DEVPROTO
	},
};

static int
ubt_match(device_t parent, cfdata_t match, void *aux)
{
	struct usb_attach_arg *uaa = aux;
	size_t i;

	DPRINTFN(50, "ubt_match\n");

	for (i = 0; i < __arraycount(ubt_dev); i++) {
		if (ubt_dev[i].vendor != -1
		    && ubt_dev[i].vendor != (int)uaa->uaa_vendor)
			continue;
		if (ubt_dev[i].product != -1
		    && ubt_dev[i].product != (int)uaa->uaa_product)
			continue;
		if (ubt_dev[i].class != -1
		    && ubt_dev[i].class != uaa->uaa_class)
			continue;
		if (ubt_dev[i].subclass != -1
		    && ubt_dev[i].subclass != uaa->uaa_subclass)
			continue;
		if (ubt_dev[i].proto != -1
		    && ubt_dev[i].proto != uaa->uaa_proto)
			continue;

		return ubt_dev[i].match;
	}

	return UMATCH_NONE;
}

static void
ubt_attach(device_t parent, device_t self, void *aux)
{
	struct ubt_softc *sc = device_private(self);
	struct usb_attach_arg *uaa = aux;
	usb_config_descriptor_t *cd;
	usb_endpoint_descriptor_t *ed;
	const struct sysctlnode *node;
	char *devinfop;
	int err;
	uint8_t count, i;

	DPRINTFN(50, "ubt_attach: sc=%p\n", sc);

	sc->sc_dev = self;
	sc->sc_udev = uaa->uaa_device;
	sc->sc_intel = (uaa->uaa_vendor == UBT_INTEL_VENDOR);

	MBUFQ_INIT(&sc->sc_cmd_queue);
	MBUFQ_INIT(&sc->sc_aclwr_queue);
	MBUFQ_INIT(&sc->sc_scowr_queue);

	aprint_naive("\n");
	aprint_normal("\n");

	devinfop = usbd_devinfo_alloc(sc->sc_udev, 0);
	aprint_normal_dev(self, "%s\n", devinfop);
	usbd_devinfo_free(devinfop);

	/*
	 * Move the device into the configured state
	 */
	err = usbd_set_config_index(sc->sc_udev, 0, 1);
	if (err) {
		aprint_error_dev(self,
		    "failed to set configuration idx 0: %s\n",
		    usbd_errstr(err));

		return;
	}

	/*
	 * Interface 0 must have 3 endpoints
	 *	1) Interrupt endpoint to receive HCI events
	 *	2) Bulk IN endpoint to receive ACL data
	 *	3) Bulk OUT endpoint to send ACL data
	 */
	err = usbd_device2interface_handle(sc->sc_udev, 0, &sc->sc_iface0);
	if (err) {
		aprint_error_dev(self,
		    "Could not get interface 0 handle %s (%d)\n",
		    usbd_errstr(err), err);

		return;
	}

	sc->sc_evt_addr = -1;
	sc->sc_aclrd_addr = -1;
	sc->sc_aclwr_addr = -1;

	count = 0;
	(void)usbd_endpoint_count(sc->sc_iface0, &count);

	for (i = 0 ; i < count ; i++) {
		int dir, type;

		ed = usbd_interface2endpoint_descriptor(sc->sc_iface0, i);
		if (ed == NULL) {
			aprint_error_dev(self,
			    "could not read endpoint descriptor %d\n", i);

			return;
		}

		dir = UE_GET_DIR(ed->bEndpointAddress);
		type = UE_GET_XFERTYPE(ed->bmAttributes);

		if (dir == UE_DIR_IN && type == UE_INTERRUPT)
			sc->sc_evt_addr = ed->bEndpointAddress;
		else if (dir == UE_DIR_IN && type == UE_BULK)
			sc->sc_aclrd_addr = ed->bEndpointAddress;
		else if (dir == UE_DIR_OUT && type == UE_BULK)
			sc->sc_aclwr_addr = ed->bEndpointAddress;
	}

	if (sc->sc_evt_addr == -1) {
		aprint_error_dev(self,
		    "missing INTERRUPT endpoint on interface 0\n");

		return;
	}
	if (sc->sc_aclrd_addr == -1) {
		aprint_error_dev(self,
		    "missing BULK IN endpoint on interface 0\n");

		return;
	}
	if (sc->sc_aclwr_addr == -1) {
		aprint_error_dev(self,
		    "missing BULK OUT endpoint on interface 0\n");

		return;
	}

	/*
	 * Interface 1 must have 2 endpoints
	 *	1) Isochronous IN endpoint to receive SCO data
	 *	2) Isochronous OUT endpoint to send SCO data
	 *
	 * and will have several configurations, which can be selected
	 * via a sysctl variable. We select config 0 to start, which
	 * means that no SCO data will be available.
	 */
	err = usbd_device2interface_handle(sc->sc_udev, 1, &sc->sc_iface1);
	if (err) {
		aprint_error_dev(self,
		    "Could not get interface 1 handle %s (%d)\n",
		    usbd_errstr(err), err);

		return;
	}

	cd = usbd_get_config_descriptor(sc->sc_udev);
	if (cd == NULL) {
		aprint_error_dev(self, "could not get config descriptor\n");

		return;
	}

	sc->sc_alt_config = usbd_get_no_alts(cd, 1);

	/* set initial config */
	err = ubt_set_isoc_config(sc);
	if (err) {
		aprint_error_dev(self, "ISOC config failed\n");

		return;
	}

	/* Attach HCI */
	sc->sc_unit = hci_attach_pcb(&ubt_hci, sc->sc_dev, 0);

	usbd_add_drv_event(USB_EVENT_DRIVER_ATTACH, sc->sc_udev, sc->sc_dev);

	/* sysctl set-up for alternate configs */
	sysctl_createv(&sc->sc_log, 0, NULL, &node,
		0,
		CTLTYPE_NODE, device_xname(sc->sc_dev),
		SYSCTL_DESCR("ubt driver information"),
		NULL, 0,
		NULL, 0,
		CTL_HW,
		CTL_CREATE, CTL_EOL);

	if (node != NULL) {
		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "config",
			SYSCTL_DESCR("configuration number"),
			ubt_sysctl_config, 0,
			(void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READONLY,
			CTLTYPE_INT, "alt_config",
			SYSCTL_DESCR("number of alternate configurations"),
			NULL, 0,
			&sc->sc_alt_config, sizeof(sc->sc_alt_config),
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READONLY,
			CTLTYPE_INT, "sco_rxsize",
			SYSCTL_DESCR("max SCO receive size"),
			NULL, 0,
			&sc->sc_scord_size, sizeof(sc->sc_scord_size),
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READONLY,
			CTLTYPE_INT, "sco_txsize",
			SYSCTL_DESCR("max SCO transmit size"),
			NULL, 0,
			&sc->sc_scowr_size, sizeof(sc->sc_scowr_size),
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);
	}

	sc->sc_ok = 1;

	if (!pmf_device_register(self, ubt_suspend, ubt_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");

	return;
}

/*
 * LISPBSD: USB runtime power management.  The xhci idle scan parks a
 * device whose driver can let go of its pipes: here the three inbound
 * pipes (events, ACL, SCO) are aborted but stay allocated, the port then
 * goes to U3 with remote wakeup armed.  Commands and ACL writes keep
 * working while parked (their doorbells are deferred and wake the port),
 * and an event from the controller wakes it by itself.  resume reopens
 * the event pipe (an interrupt pipe cannot be restarted after an abort)
 * and re-queues the reads.  The same handlers serve system suspend.
 */
static bool
ubt_suspend(device_t self, const pmf_qual_t *qual)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	if (!sc->sc_enabled || sc->sc_parked || sc->sc_dying)
		return true;
	s = splusb();
	sc->sc_parked = 1;
	if (sc->sc_evt_pipe != NULL)
		usbd_abort_pipe(sc->sc_evt_pipe);
	if (sc->sc_aclrd_pipe != NULL)
		usbd_abort_pipe(sc->sc_aclrd_pipe);
	if (sc->sc_scord_pipe != NULL)
		usbd_abort_pipe(sc->sc_scord_pipe);
	splx(s);
	DPRINTFN(1, "sc=%p parked\n", sc);
	return true;
}

static bool
ubt_resume(device_t self, const pmf_qual_t *qual)
{
	struct ubt_softc *sc = device_private(self);
	usbd_status err;
	int i, s;

	if (!sc->sc_enabled || !sc->sc_parked || sc->sc_dying)
		return true;
	s = splusb();
	/*
	 * Re-arm the event pipe in place.  A close and reopen would reset the
	 * host's data toggle but not the controller's, and the next event,
	 * the reply to the command that woke the port, would be discarded:
	 * one HCI command credit lost per wake (num_cmd 0 = dead radio).
	 * Fall back to a reopen only if the pipe is gone.
	 */
	if (sc->sc_evt_pipe != NULL) {
		err = usbd_restart_pipe_intr(sc->sc_evt_pipe);
		if (err != USBD_NORMAL_COMPLETION) {
			/* The retry reopens it (resynchronising the toggle is
			 * then left to luck, as before). */
			usbd_close_pipe(sc->sc_evt_pipe);
			sc->sc_evt_pipe = NULL;
		}
	} else
		err = usbd_open_pipe_intr(sc->sc_iface0, sc->sc_evt_addr,
		    USBD_SHORT_XFER_OK, &sc->sc_evt_pipe, sc, sc->sc_evt_buf,
		    UBT_BUFSIZ_EVENT, ubt_recv_event, USBD_DEFAULT_INTERVAL);
	/* A failed restart or reopen leaves sc_parked set for retry. */
	if (err != USBD_NORMAL_COMPLETION) {
		sc->sc_evt_pipe = NULL; /* open_pipe_intr may publish then free it */
		splx(s);
		aprint_error_dev(self,
		    "event pipe did not reopen after suspend: %s\n",
		    usbd_errstr(err));
		return false;
	}
	sc->sc_parked = 0;
	if (sc->sc_aclrd_pipe != NULL)
		ubt_recv_acl_start(sc);
	if (sc->sc_scord_pipe != NULL && sc->sc_scord_size != 0)
		for (i = 0; i < UBT_NXFERS; i++)
			ubt_recv_sco_start1(sc, &sc->sc_scord[i]);
	splx(s);
	DPRINTFN(1, "sc=%p unparked\n", sc);
	return true;
}

static int
ubt_detach(device_t self, int flags)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	DPRINTF("sc=%p flags=%d\n", sc, flags);

	pmf_device_deregister(self);

	sc->sc_dying = 1;

	if (!sc->sc_ok)
		return 0;

	/* delete sysctl nodes */
	sysctl_teardown(&sc->sc_log);

	/* Detach HCI interface */
	if (sc->sc_unit) {
		hci_detach_pcb(sc->sc_unit);
		sc->sc_unit = NULL;
	}

	/*
	 * Abort all pipes. Causes processes waiting for transfer to wake.
	 *
	 * Actually, hci_detach_pcb() above will call ubt_disable() which
	 * may call ubt_abortdealloc(), but lets be sure since doing it
	 * twice wont cause an error.
	 */
	ubt_abortdealloc(sc);

	/* wait for all processes to finish */
	s = splusb();
	if (sc->sc_refcnt-- > 0)
		usb_detach_waitold(sc->sc_dev);

	splx(s);

	usbd_add_drv_event(USB_EVENT_DRIVER_DETACH, sc->sc_udev, sc->sc_dev);

	DPRINTFN(1, "driver detached\n");

	return 0;
}

static int
ubt_activate(device_t self, enum devact act)
{
	struct ubt_softc *sc = device_private(self);

	DPRINTFN(1, "sc=%p, act=%d\n", sc, act);

	switch (act) {
	case DVACT_DEACTIVATE:
		sc->sc_dying = 1;
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

/* set ISOC configuration */
static int
ubt_set_isoc_config(struct ubt_softc *sc)
{
	usb_endpoint_descriptor_t *ed;
	int rd_addr, wr_addr, rd_size, wr_size;
	uint8_t count, i;
	int err;

	err = usbd_set_interface(sc->sc_iface1, sc->sc_config);
	if (err != USBD_NORMAL_COMPLETION) {
		aprint_error_dev(sc->sc_dev,
		    "Could not set config %d on ISOC interface. %s (%d)\n",
		    sc->sc_config, usbd_errstr(err), err);

		return err == USBD_IN_USE ? EBUSY : EIO;
	}

	/*
	 * We wont get past the above if there are any pipes open, so no
	 * need to worry about buf/xfer/pipe deallocation. If we get an
	 * error after this, the frame quantities will be 0 and no SCO
	 * data will be possible.
	 */

	sc->sc_scord_size = rd_size = 0;
	sc->sc_scord_addr = rd_addr = -1;

	sc->sc_scowr_size = wr_size = 0;
	sc->sc_scowr_addr = wr_addr = -1;

	count = 0;
	(void)usbd_endpoint_count(sc->sc_iface1, &count);

	for (i = 0 ; i < count ; i++) {
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface1, i);
		if (ed == NULL) {
			aprint_error_dev(sc->sc_dev,
			    "could not read endpoint descriptor %d\n", i);

			return EIO;
		}

		DPRINTFN(5, "%s: endpoint type %02x (%02x) addr %02x (%s)\n",
			device_xname(sc->sc_dev),
			UE_GET_XFERTYPE(ed->bmAttributes),
			UE_GET_ISO_TYPE(ed->bmAttributes),
			ed->bEndpointAddress,
			UE_GET_DIR(ed->bEndpointAddress) ? "in" : "out");

		if (UE_GET_XFERTYPE(ed->bmAttributes) != UE_ISOCHRONOUS)
			continue;

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN) {
			rd_addr = ed->bEndpointAddress;
			rd_size = UGETW(ed->wMaxPacketSize);
		} else {
			wr_addr = ed->bEndpointAddress;
			wr_size = UGETW(ed->wMaxPacketSize);
		}
	}

	if (rd_addr == -1) {
		aprint_error_dev(sc->sc_dev,
		    "missing ISOC IN endpoint on interface config %d\n",
		    sc->sc_config);

		return ENOENT;
	}
	if (wr_addr == -1) {
		aprint_error_dev(sc->sc_dev,
		    "missing ISOC OUT endpoint on interface config %d\n",
		    sc->sc_config);

		return ENOENT;
	}

	if (rd_size > MLEN) {
		aprint_error_dev(sc->sc_dev, "rd_size=%d exceeds MLEN\n",
		    rd_size);

		return EOVERFLOW;
	}

	if (wr_size > MLEN) {
		aprint_error_dev(sc->sc_dev, "wr_size=%d exceeds MLEN\n",
		    wr_size);

		return EOVERFLOW;
	}

	sc->sc_scord_size = rd_size;
	sc->sc_scord_addr = rd_addr;

	sc->sc_scowr_size = wr_size;
	sc->sc_scowr_addr = wr_addr;

	return 0;
}

/* sysctl helper to set alternate configurations */
static int
ubt_sysctl_config(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct ubt_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_config;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	if (t < 0 || t >= sc->sc_alt_config)
		return EINVAL;

	/* This may not change when the unit is enabled */
	if (sc->sc_enabled)
		return EBUSY;

	KERNEL_LOCK(1, curlwp);
	sc->sc_config = t;
	error = ubt_set_isoc_config(sc);
	KERNEL_UNLOCK_ONE(curlwp);
	return error;
}

static void
ubt_abortdealloc(struct ubt_softc *sc)
{
	int i;

	DPRINTFN(1, "sc=%p\n", sc);

	/* Abort all pipes */
	usbd_abort_default_pipe(sc->sc_udev);

	if (sc->sc_evt_pipe != NULL) {
		usbd_abort_pipe(sc->sc_evt_pipe);
	}

	if (sc->sc_aclrd_pipe != NULL) {
		usbd_abort_pipe(sc->sc_aclrd_pipe);
	}

	if (sc->sc_aclwr_pipe != NULL) {
		usbd_abort_pipe(sc->sc_aclwr_pipe);
	}

	if (sc->sc_scord_pipe != NULL) {
		usbd_abort_pipe(sc->sc_scord_pipe);
	}

	if (sc->sc_scowr_pipe != NULL) {
		usbd_abort_pipe(sc->sc_scowr_pipe);
	}

	/* Free event buffer */
	if (sc->sc_evt_buf != NULL) {
		kmem_free(sc->sc_evt_buf, UBT_BUFSIZ_EVENT);
		sc->sc_evt_buf = NULL;
	}

	/* Free all xfers and xfer buffers (implicit) */
	if (sc->sc_cmd_xfer != NULL) {
		usbd_destroy_xfer(sc->sc_cmd_xfer);
		sc->sc_cmd_xfer = NULL;
		sc->sc_cmd_buf = NULL;
	}

	if (sc->sc_aclrd_xfer != NULL) {
		usbd_destroy_xfer(sc->sc_aclrd_xfer);
		sc->sc_aclrd_xfer = NULL;
		sc->sc_aclrd_buf = NULL;
	}

	if (sc->sc_aclwr_xfer != NULL) {
		usbd_destroy_xfer(sc->sc_aclwr_xfer);
		sc->sc_aclwr_xfer = NULL;
		sc->sc_aclwr_buf = NULL;
	}

	for (i = 0 ; i < UBT_NXFERS ; i++) {
		if (sc->sc_scord[i].xfer != NULL) {
			usbd_destroy_xfer(sc->sc_scord[i].xfer);
			sc->sc_scord[i].xfer = NULL;
			sc->sc_scord[i].buf = NULL;
		}

		if (sc->sc_scowr[i].xfer != NULL) {
			usbd_destroy_xfer(sc->sc_scowr[i].xfer);
			sc->sc_scowr[i].xfer = NULL;
			sc->sc_scowr[i].buf = NULL;
		}
	}

	if (sc->sc_evt_pipe != NULL) {
		usbd_close_pipe(sc->sc_evt_pipe);
		sc->sc_evt_pipe = NULL;
	}

	if (sc->sc_aclrd_pipe != NULL) {
		usbd_close_pipe(sc->sc_aclrd_pipe);
		sc->sc_aclrd_pipe = NULL;
	}

	if (sc->sc_aclwr_pipe != NULL) {
		usbd_close_pipe(sc->sc_aclwr_pipe);
		sc->sc_aclwr_pipe = NULL;
	}

	if (sc->sc_scord_pipe != NULL) {
		usbd_close_pipe(sc->sc_scord_pipe);
		sc->sc_scord_pipe = NULL;
	}

	if (sc->sc_scowr_pipe != NULL) {
		usbd_close_pipe(sc->sc_scowr_pipe);
		sc->sc_scowr_pipe = NULL;
	}

	/* Free partial SCO packets */
	m_freem(sc->sc_scord_mbuf);
	sc->sc_scord_mbuf = NULL;

	m_freem(sc->sc_scowr_mbuf);
	sc->sc_scowr_mbuf = NULL;

	/* Empty mbuf queues */
	MBUFQ_DRAIN(&sc->sc_cmd_queue);
	MBUFQ_DRAIN(&sc->sc_aclwr_queue);
	MBUFQ_DRAIN(&sc->sc_scowr_queue);
}

/*******************************************************************************
 *
 * Bluetooth Unit/USB callbacks
 *
 */

/*******************************************************************************
 *
 * LISPBSD: Intel firmware download
 *
 * Intel CNVi controllers (JfP, ThP, HrP, CcP: hardware variants 0x11 to
 * 0x14, e.g. the AX201 as USB 8087:0026) power up in a bootloader that
 * only takes the vendor commands needed to load the operational firmware;
 * HCI Reset answers "unknown command" and the netbt init never completes.
 * Linux's btintel does the download from its setup hook.  Here it happens
 * on the first enable, when the root file system is there to hold the
 * image: /libdata/firmware/ubt/ibt-<hw_variant>-<hw_revision>-<fw_revision>.sfi
 * and .ddc, the files linux-firmware ships under intel/.
 *
 * The exchange is synchronous and uses nothing of the netbt stack:
 * commands go down the control pipe exactly as ubt_xmit_cmd sends them,
 * events are read one at a time from the interrupt endpoint.  The
 * bootloader acknowledges every fragment with a Command Complete (the
 * boot parameters say so, limited_cce == 0) and reports the result of the
 * whole download, and later the boot into the new firmware, in vendor
 * events 0xff/0x06 and 0xff/0x02.
 */

struct ubt_intel_version {
	uint8_t		status;
	uint8_t		hw_platform;	/* 0x37 */
	uint8_t		hw_variant;
	uint8_t		hw_revision;
	uint8_t		fw_variant;	/* 0x06 bootloader, 0x23 firmware */
	uint8_t		fw_revision;
	uint8_t		fw_build_num;
	uint8_t		fw_build_ww;
	uint8_t		fw_build_yy;
	uint8_t		fw_patch_num;
} __packed;

struct ubt_intel_boot_params {
	uint8_t		status;
	uint8_t		otp_format;
	uint8_t		otp_content;
	uint8_t		otp_patch;
	uint8_t		dev_revid[2];
	uint8_t		secure_boot;
	uint8_t		key_from_hdr;
	uint8_t		key_type;
	uint8_t		otp_lock;
	uint8_t		api_lock;
	uint8_t		debug_lock;
	uint8_t		otp_bdaddr[6];
	uint8_t		min_fw_build_nn;
	uint8_t		min_fw_build_cw;
	uint8_t		min_fw_build_yy;
	uint8_t		limited_cce;
	uint8_t		unlocked_state;
} __packed;

#define UBT_INTEL_CMD_READ_VERSION	0xfc05
#define UBT_INTEL_CMD_READ_BOOT_PARAMS	0xfc0d
#define UBT_INTEL_CMD_WRITE_BOOT_PARAMS	0xfc0e	/* inside the firmware image */
#define UBT_INTEL_CMD_SECURE_SEND	0xfc09
#define UBT_INTEL_CMD_RESET		0xfc01
#define UBT_INTEL_CMD_WRITE_DDC		0xfc8b
#define UBT_INTEL_CMD_EVENT_MASK	0xfc52

#define UBT_INTEL_RSA_HEADER_LEN	644	/* 128 CSS + 256 key + 256 signature */
#define UBT_INTEL_FRAGMENT		252
#define UBT_INTEL_MAX_FW		(4 * 1024 * 1024)
#define UBT_INTEL_MAX_DDC		4096

#define UBT_INTEL_NEVQ		16

struct ubt_intel_ev {
	uint32_t		 len;
	uint8_t			 data[UBT_BUFSIZ_EVENT];
};

/*
 * The loader uses the pipes ubt_enable opened: while sc_intel_loading is
 * set, ubt_recv_event and ubt_recv_acl_complete hand their packets here
 * instead of to netbt, since the bootloader answers commands sent down
 * the bulk endpoint on the bulk-in endpoint.  Nothing is closed and
 * reopened around the download, as in btusb; the endpoints keep their
 * state into the HCI init that follows.
 */
struct ubt_intel {
	struct ubt_softc	*sc;
	kmutex_t		 lock;
	kcondvar_t		 cv;
	struct ubt_intel_ev	 evq[UBT_INTEL_NEVQ];
	unsigned		 evq_head;	/* next to read */
	unsigned		 evq_count;
	struct usbd_xfer	*wxfer;		/* on the bulk-out pipe */
	uint8_t			*cmd;		/* 3 + 255, the bulk xfer's buffer */
	uint8_t			 evt[UBT_BUFSIZ_EVENT];	/* last event read */
	int			 ssr_seen;	/* vendor 0x06: download result */
	uint8_t			 ssr_result;
	int			 boot_seen;	/* vendor 0x02: firmware booted */
};

static void
ubt_intel_enqueue(struct ubt_intel *ui, const uint8_t *buf, uint32_t len)
{
	struct ubt_intel_ev *ev;

	if (ui == NULL || len < 2 || len > UBT_BUFSIZ_EVENT)
		return;
	mutex_enter(&ui->lock);
	if (ui->evq_count < UBT_INTEL_NEVQ) {
		ev = &ui->evq[(ui->evq_head + ui->evq_count) % UBT_INTEL_NEVQ];
		memcpy(ev->data, buf, len);
		ev->len = len;
		ui->evq_count++;
		cv_broadcast(&ui->cv);
	}
	mutex_exit(&ui->lock);
}

/*
 * Take the next event from the queue into ui->evt; note the bootloader's
 * vendor events on the way.
 */
static int
ubt_intel_read_event(struct ubt_intel *ui, int timeout_ms, uint32_t *lenp)
{
	struct ubt_intel_ev *ev;
	uint32_t len;

	if (ui->sc->sc_dying)
		return EIO;
	mutex_enter(&ui->lock);
	while (ui->evq_count == 0) {
		if (cv_timedwait(&ui->cv, &ui->lock,
		    MAX(1, mstohz(timeout_ms))) == EWOULDBLOCK) {
			mutex_exit(&ui->lock);
			return ETIMEDOUT;
		}
		if (ui->sc->sc_dying) {
			mutex_exit(&ui->lock);
			return EIO;
		}
	}
	ev = &ui->evq[ui->evq_head];
	len = ev->len;
	memcpy(ui->evt, ev->data, len);
	ui->evq_head = (ui->evq_head + 1) % UBT_INTEL_NEVQ;
	ui->evq_count--;
	mutex_exit(&ui->lock);

	if (len < 2u + ui->evt[1])
		return EIO;
	if (ui->evt[0] == 0xff && ui->evt[1] >= 1) {
		switch (ui->evt[2]) {
		case 0x02:
			ui->boot_seen = 1;
			break;
		case 0x06:
			if (ui->evt[1] >= 5) {
				ui->ssr_seen = 1;
				ui->ssr_result = ui->evt[3];
			}
			break;
		}
	}
	*lenp = len;
	return 0;
}

static int
ubt_intel_ms_since(const struct timeval *start)
{
	struct timeval now, d;

	getmicrouptime(&now);
	timersub(&now, start, &d);
	return d.tv_sec * 1000 + d.tv_usec / 1000;
}

/*
 * Send one HCI command and wait for its Command Complete.  The return
 * parameters (status first) are copied to rsp, *rsplen in/out.  A Command
 * Status with an error ends the wait; anything else is dropped (vendor
 * events having been noted by ubt_intel_read_event).
 *
 * Two bootloader quirks, as in Linux's btusb: Intel_Secure_Send goes down
 * the bulk endpoint rather than the control pipe, and Intel_Reset never
 * answers (timeout_ms 0: send and return).
 */
static int
ubt_intel_cmd(struct ubt_intel *ui, uint16_t opcode, const void *params,
    uint8_t plen, void *rsp, size_t *rsplen, int timeout_ms)
{
	usb_device_request_t req;
	struct timeval start;
	usbd_status err;
	uint32_t len;
	size_t n;
	int error, left;

	ui->cmd[0] = opcode & 0xff;
	ui->cmd[1] = opcode >> 8;
	ui->cmd[2] = plen;
	if (plen > 0)
		memcpy(&ui->cmd[3], params, plen);

	if (opcode == UBT_INTEL_CMD_SECURE_SEND) {
		len = 3 + plen;
		err = usbd_bulk_transfer(ui->wxfer, ui->sc->sc_aclwr_pipe, 0,
		    2000, ui->cmd, &len);
	} else {
		memset(&req, 0, sizeof(req));
		req.bmRequestType = UT_WRITE_CLASS_DEVICE;
		USETW(req.wLength, 3 + plen);
		err = usbd_do_request(ui->sc->sc_udev, &req, ui->cmd);
	}
	if (err != USBD_NORMAL_COMPLETION) {
		aprint_error_dev(ui->sc->sc_dev,
		    "Intel command %#x: %s\n", opcode, usbd_errstr(err));
		return EIO;
	}
	if (timeout_ms == 0)
		return 0;

	getmicrouptime(&start);
	for (;;) {
		left = timeout_ms - ubt_intel_ms_since(&start);
		if (left <= 0)
			return ETIMEDOUT;
		error = ubt_intel_read_event(ui, left, &len);
		if (error)
			return error;
		/* Command Complete: ncmd, opcode, return parameters */
		if (ui->evt[0] == 0x0e && len >= 5 &&
		    (ui->evt[3] | (ui->evt[4] << 8)) == opcode) {
			n = len - 5;
			if (rsp != NULL) {
				if (n > *rsplen)
					n = *rsplen;
				memcpy(rsp, &ui->evt[5], n);
				*rsplen = n;
			}
			return 0;
		}
		/* Command Status: status, ncmd, opcode */
		if (ui->evt[0] == 0x0f && len >= 6 &&
		    (ui->evt[4] | (ui->evt[5] << 8)) == opcode &&
		    ui->evt[2] != 0) {
			aprint_error_dev(ui->sc->sc_dev,
			    "Intel command %#x: status %#x\n", opcode,
			    ui->evt[2]);
			return EIO;
		}
	}
}

static int
ubt_intel_wait(struct ubt_intel *ui, int *flag, int timeout_ms)
{
	struct timeval start;
	uint32_t len;
	int left, error;

	getmicrouptime(&start);
	while (!*flag) {
		left = timeout_ms - ubt_intel_ms_since(&start);
		if (left <= 0)
			return ETIMEDOUT;
		error = ubt_intel_read_event(ui, left, &len);
		if (error)
			return error;
	}
	return 0;
}

/*
 * Intel_Secure_Send: fragment type, then up to 252 bytes of the image.
 */
static int
ubt_intel_secure_send(struct ubt_intel *ui, uint8_t type,
    const uint8_t *data, size_t len)
{
	uint8_t params[1 + UBT_INTEL_FRAGMENT], status;
	size_t n, rl;
	int error;

	while (len > 0) {
		n = MIN(len, UBT_INTEL_FRAGMENT);
		params[0] = type;
		memcpy(&params[1], data, n);
		rl = 1;
		status = 0xff;
		error = ubt_intel_cmd(ui, UBT_INTEL_CMD_SECURE_SEND, params,
		    n + 1, &status, &rl, 2000);
		if (error)
			return error;
		if (rl < 1 || status != 0) {
			aprint_error_dev(ui->sc->sc_dev,
			    "Intel secure send type %#x: status %#x\n", type,
			    status);
			return EIO;
		}
		data += n;
		len -= n;
	}
	return 0;
}

/*
 * Intel_Read_Version.  With the 0xff parameter every bootloader answers
 * in the legacy layout; operational firmware answers that form with TLVs
 * (platform byte not 0x37), so the plain form is used once it runs.
 */
static int
ubt_intel_read_version(struct ubt_intel *ui, struct ubt_intel_version *ver,
    int with_param)
{
	uint8_t param = 0xff;
	size_t rl = sizeof(*ver);
	int error;

	error = ubt_intel_cmd(ui, UBT_INTEL_CMD_READ_VERSION, &param,
	    with_param ? 1 : 0, ver, &rl, 2000);
	if (error)
		return error;
	if (rl != sizeof(*ver) || ver->status != 0 || ver->hw_platform != 0x37) {
		aprint_error_dev(ui->sc->sc_dev,
		    "unexpected Intel version reply (%zu bytes, status %#x, "
		    "platform %#x)\n", rl, ver->status, ver->hw_platform);
		return EIO;
	}
	return 0;
}

static int
ubt_intel_fwload(struct ubt_softc *sc, int *booted)
{
	struct ubt_intel *ui;
	struct ubt_intel_version ver;
	struct ubt_intel_boot_params bp;
	struct timeval start;
	firmware_handle_t fwh;
	char name[32];
	uint8_t *fw = NULL, *ddc, status, reset[8];
	size_t fwsize = 0, dsz, rl, off, frag;
	uint32_t boot_param = 0;
	int error;

	ui = kmem_zalloc(sizeof(*ui), KM_SLEEP);
	ui->sc = sc;
	mutex_init(&ui->lock, MUTEX_DEFAULT, IPL_USB);
	cv_init(&ui->cv, "ubtintel");
	error = usbd_create_xfer(sc->sc_aclwr_pipe, 3 + 255, 0, 0, &ui->wxfer);
	if (error) {
		cv_destroy(&ui->cv);
		mutex_destroy(&ui->lock);
		kmem_free(ui, sizeof(*ui));
		return error;
	}
	ui->cmd = usbd_get_buffer(ui->wxfer);
	sc->sc_intel_ui = ui;
	sc->sc_intel_loading = 1;

	error = ubt_intel_read_version(ui, &ver, 1);
	if (error) {
		if (ver.status == 0 && ver.hw_platform != 0x37) {
			/* A TLV answer: operational firmware is running. */
			aprint_normal_dev(sc->sc_dev,
			    "Intel firmware already running\n");
			error = 0;
		}
		goto out;
	}
	aprint_normal_dev(sc->sc_dev,
	    "Intel hw variant %#x rev %u, %s revision %u.%u build %u week %u %u\n",
	    ver.hw_variant, ver.hw_revision,
	    ver.fw_variant == 0x06 ? "bootloader" :
	    ver.fw_variant == 0x23 ? "firmware" : "unknown image",
	    ver.fw_revision >> 4, ver.fw_revision & 0x0f, ver.fw_build_num,
	    ver.fw_build_ww, 2000 + ver.fw_build_yy);
	if (ver.fw_variant == 0x23) {
		/* Operational firmware is running (warm restart). */
		error = 0;
		goto out;
	}
	if (ver.fw_variant != 0x06) {
		error = ENXIO;
		goto out;
	}
	switch (ver.hw_variant) {
	case 0x11:	/* JfP */
	case 0x12:	/* ThP */
	case 0x13:	/* HrP */
	case 0x14:	/* CcP */
		break;
	default:
		aprint_error_dev(sc->sc_dev,
		    "no firmware loader for Intel hw variant %#x\n",
		    ver.hw_variant);
		error = ENXIO;
		goto out;
	}

	rl = sizeof(bp);
	error = ubt_intel_cmd(ui, UBT_INTEL_CMD_READ_BOOT_PARAMS, NULL, 0, &bp,
	    &rl, 2000);
	if (error == 0 && (rl != sizeof(bp) || bp.status != 0))
		error = EIO;
	if (error) {
		aprint_error_dev(sc->sc_dev, "Intel boot parameters: %d\n",
		    error);
		goto out;
	}
	if (bp.limited_cce != 0) {
		aprint_error_dev(sc->sc_dev,
		    "Intel bootloader does not acknowledge fragments (%u)\n",
		    bp.limited_cce);
		error = ENXIO;
		goto out;
	}
	aprint_normal_dev(sc->sc_dev,
	    "Intel device revision %u, secure boot %s, otp bdaddr %s\n",
	    bp.dev_revid[0] | (bp.dev_revid[1] << 8),
	    bp.secure_boot ? "on" : "off",
	    (bp.otp_bdaddr[0] | bp.otp_bdaddr[1] | bp.otp_bdaddr[2] |
	     bp.otp_bdaddr[3] | bp.otp_bdaddr[4] | bp.otp_bdaddr[5]) ?
	    "set" : "none");

	snprintf(name, sizeof(name), "ibt-%u-%u-%u.sfi", ver.hw_variant,
	    ver.hw_revision, ver.fw_revision);
	error = firmware_open("ubt", name, &fwh);
	if (error) {
		aprint_error_dev(sc->sc_dev,
		    "firmware %s not found (%d); install it in "
		    "/libdata/firmware/ubt\n", name, error);
		goto out;
	}
	fwsize = firmware_get_size(fwh);
	if (fwsize < UBT_INTEL_RSA_HEADER_LEN + 4 || fwsize > UBT_INTEL_MAX_FW) {
		aprint_error_dev(sc->sc_dev, "firmware %s: bad size %zu\n",
		    name, fwsize);
		firmware_close(fwh);
		error = EINVAL;
		fwsize = 0;
		goto out;
	}
	fw = firmware_malloc(fwsize);
	error = firmware_read(fwh, 0, fw, fwsize);
	firmware_close(fwh);
	if (error) {
		aprint_error_dev(sc->sc_dev, "firmware %s: read error %d\n",
		    name, error);
		goto out;
	}

	/*
	 * Walk the command stream behind the signature block once: every
	 * command must lie inside the file, and Intel_Write_Boot_Params in
	 * it carries the address the firmware is booted from.
	 */
	off = UBT_INTEL_RSA_HEADER_LEN;
	while (off < fwsize) {
		uint16_t op;
		uint8_t plen;

		if (off + 3 > fwsize)
			goto badfw;
		op = fw[off] | (fw[off + 1] << 8);
		plen = fw[off + 2];
		if (off + 3 + plen > fwsize)
			goto badfw;
		if (op == UBT_INTEL_CMD_WRITE_BOOT_PARAMS && plen >= 7) {
			boot_param = fw[off + 3] | (fw[off + 4] << 8) |
			    (fw[off + 5] << 16) | ((uint32_t)fw[off + 6] << 24);
			aprint_normal_dev(sc->sc_dev,
			    "firmware %s: build %u week %u %u, boot address %#x\n",
			    name, fw[off + 7], fw[off + 8], 2000 + fw[off + 9],
			    boot_param);
		}
		off += 3 + plen;
	}

	getmicrouptime(&start);

	/* CSS header, public key and signature open the download. */
	if ((error = ubt_intel_secure_send(ui, 0x00, fw, 128)) != 0 ||
	    (error = ubt_intel_secure_send(ui, 0x03, fw + 128, 256)) != 0 ||
	    (error = ubt_intel_secure_send(ui, 0x02, fw + 388, 256)) != 0) {
		aprint_error_dev(sc->sc_dev, "firmware header rejected (%d)\n",
		    error);
		goto reset;
	}

	/*
	 * Then the commands, in groups whose length is a multiple of four
	 * as the file lays them out (Intel_NOPs pad them).
	 */
	off = UBT_INTEL_RSA_HEADER_LEN;
	frag = 0;
	while (off + frag < fwsize) {
		frag += 3 + fw[off + frag + 2];
		if ((frag % 4) == 0) {
			error = ubt_intel_secure_send(ui, 0x01, fw + off, frag);
			if (error) {
				aprint_error_dev(sc->sc_dev,
				    "firmware data rejected at %zu (%d)\n", off,
				    error);
				goto reset;
			}
			off += frag;
			frag = 0;
		}
	}
	if (frag != 0) {
		aprint_error_dev(sc->sc_dev,
		    "firmware %s: %zu unaligned trailing bytes\n", name, frag);
		error = EINVAL;
		goto reset;
	}

	error = ubt_intel_wait(ui, &ui->ssr_seen, 5000);
	if (error == 0 && ui->ssr_result != 0) {
		aprint_error_dev(sc->sc_dev, "firmware download failed (%#x)\n",
		    ui->ssr_result);
		error = EIO;
	} else if (error)
		aprint_error_dev(sc->sc_dev,
		    "no firmware download result (%d)\n", error);
	if (error)
		goto reset;
	aprint_normal_dev(sc->sc_dev, "firmware loaded in %d ms\n",
	    ubt_intel_ms_since(&start));

	/*
	 * Intel_Reset: soft reset, patches enabled, no DDC reload, boot from
	 * the address found above.  The bootloader does not answer; the
	 * firmware announces itself with the boot-up vendor event.
	 */
	getmicrouptime(&start);
	reset[0] = 0x00;
	reset[1] = 0x01;
	reset[2] = 0x00;
	reset[3] = 0x01;
	reset[4] = boot_param & 0xff;
	reset[5] = (boot_param >> 8) & 0xff;
	reset[6] = (boot_param >> 16) & 0xff;
	reset[7] = (boot_param >> 24) & 0xff;
	error = ubt_intel_cmd(ui, UBT_INTEL_CMD_RESET, reset, sizeof(reset),
	    NULL, NULL, 0);
	if (error) {
		aprint_error_dev(sc->sc_dev, "Intel reset: %d\n", error);
		goto out;
	}
	error = ubt_intel_wait(ui, &ui->boot_seen, 3000);
	if (error) {
		aprint_error_dev(sc->sc_dev, "firmware did not boot (%d)\n",
		    error);
		goto reset;
	}
	aprint_normal_dev(sc->sc_dev, "firmware booted in %d ms\n",
	    ubt_intel_ms_since(&start));
	*booted = 1;

	/* Device configuration parameters; the firmware runs without them. */
	snprintf(name, sizeof(name), "ibt-%u-%u-%u.ddc", ver.hw_variant,
	    ver.hw_revision, ver.fw_revision);
	if (firmware_open("ubt", name, &fwh) == 0) {
		dsz = firmware_get_size(fwh);
		if (dsz > 0 && dsz <= UBT_INTEL_MAX_DDC) {
			ddc = firmware_malloc(dsz);
			if (firmware_read(fwh, 0, ddc, dsz) == 0) {
				for (off = 0; off < dsz; off += 1 + ddc[off]) {
					if (off + 1 + ddc[off] > dsz)
						break;
					rl = 1;
					if (ubt_intel_cmd(ui,
					    UBT_INTEL_CMD_WRITE_DDC, &ddc[off],
					    1 + ddc[off], &status, &rl,
					    2000) != 0) {
						aprint_error_dev(sc->sc_dev,
						    "DDC entry at %zu rejected\n",
						    off);
						break;
					}
				}
			}
			firmware_free(ddc, dsz);
		}
		firmware_close(fwh);
	} else
		aprint_normal_dev(sc->sc_dev, "no %s, running without DDC\n",
		    name);

	if (ubt_intel_read_version(ui, &ver, 0) == 0)
		aprint_normal_dev(sc->sc_dev,
		    "Intel firmware revision %u.%u build %u week %u %u\n",
		    ver.fw_revision >> 4, ver.fw_revision & 0x0f,
		    ver.fw_build_num, ver.fw_build_ww, 2000 + ver.fw_build_yy);

	/* Intel vendor events useful in operation; harmless if refused. */
	{
		static const uint8_t mask[8] =
		    { 0x87, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		rl = 1;
		(void)ubt_intel_cmd(ui, UBT_INTEL_CMD_EVENT_MASK, mask,
		    sizeof(mask), &status, &rl, 2000);
	}
	error = 0;
	goto out;

badfw:
	aprint_error_dev(sc->sc_dev,
	    "firmware %s: malformed command stream at %zu\n", name, off);
	error = EINVAL;
	goto out;

reset:
	/*
	 * Linux's recovery: a hard reset makes the controller re-enumerate
	 * with a clean bootloader; this device goes away and comes back.
	 */
	reset[0] = 0x01;
	reset[1] = 0x01;
	reset[2] = 0x01;
	reset[3] = 0x00;
	reset[4] = reset[5] = reset[6] = reset[7] = 0;
	(void)ubt_intel_cmd(ui, UBT_INTEL_CMD_RESET, reset, sizeof(reset),
	    NULL, NULL, 0);

out:
	if (fw != NULL)
		firmware_free(fw, fwsize);
	sc->sc_intel_loading = 0;
	sc->sc_intel_ui = NULL;
	usbd_destroy_xfer(ui->wxfer);
	cv_destroy(&ui->cv);
	mutex_destroy(&ui->lock);
	kmem_free(ui, sizeof(*ui));
	return error;
}

/*
 * LISPBSD: reset the device's endpoint data toggles.  The pipes opened
 * afterwards start at DATA0 on the host (xhci configures the endpoint
 * afresh), the device's toggles only on a halt clear; after a down/up, or
 * once the Intel firmware has rebooted the chip, they are out of step and
 * the first event is silently dropped, so the HCI Reset never answers.
 */
static void
ubt_clear_halts(struct ubt_softc *sc)
{

	(void)usbd_clear_endpoint_feature(sc->sc_udev, sc->sc_evt_addr,
	    UF_ENDPOINT_HALT);
	(void)usbd_clear_endpoint_feature(sc->sc_udev, sc->sc_aclrd_addr,
	    UF_ENDPOINT_HALT);
	(void)usbd_clear_endpoint_feature(sc->sc_udev, sc->sc_aclwr_addr,
	    UF_ENDPOINT_HALT);
}

static int ubt_open_pipes(struct ubt_softc *);

static int
ubt_enable(device_t self)
{
	struct ubt_softc *sc = device_private(self);
	int s, error, booted;

	DPRINTFN(1, "sc=%p\n", sc);

	if (sc->sc_enabled)
		return 0;

	ubt_clear_halts(sc);
	s = splusb();
	error = ubt_open_pipes(sc);
	if (error) {
		ubt_abortdealloc(sc);
		splx(s);
		return error;
	}
	sc->sc_enabled = 1;
	splx(s);

	/*
	 * LISPBSD: an Intel controller still in its bootloader gets the
	 * firmware now, on the pipes just opened, before the HCI Reset
	 * that follows.  Sleeping here is fine: bt_lock is adaptive.  Once
	 * the chip has rebooted into the firmware, its endpoints start
	 * over, so the pipes are set up again from scratch.
	 */
	if (sc->sc_intel && !sc->sc_intel_fwdone) {
		booted = 0;
		error = ubt_intel_fwload(sc, &booted);
		if (error == 0)
			sc->sc_intel_fwdone = 1;
		else
			aprint_error_dev(sc->sc_dev,
			    "Intel firmware setup failed (%d)\n", error);
		if (booted) {
			s = splusb();
			ubt_abortdealloc(sc);
			splx(s);
			ubt_clear_halts(sc);
			s = splusb();
			error = ubt_open_pipes(sc);
			if (error) {
				ubt_abortdealloc(sc);
				sc->sc_enabled = 0;
				splx(s);
				return error;
			}
			splx(s);
		}
	}
	return 0;
}

static int
ubt_open_pipes(struct ubt_softc *sc)
{
	usbd_status err;
	int i, error;

	/* Events */
	sc->sc_evt_buf = kmem_alloc(UBT_BUFSIZ_EVENT, KM_SLEEP);
	err = usbd_open_pipe_intr(sc->sc_iface0,
				  sc->sc_evt_addr,
				  USBD_SHORT_XFER_OK,
				  &sc->sc_evt_pipe,
				  sc,
				  sc->sc_evt_buf,
				  UBT_BUFSIZ_EVENT,
				  ubt_recv_event,
				  USBD_DEFAULT_INTERVAL);
	if (err != USBD_NORMAL_COMPLETION) {
		error = EIO;
		goto bad;
	}

	/* Commands */
	struct usbd_pipe *pipe0 = usbd_get_pipe0(sc->sc_udev);
	error = usbd_create_xfer(pipe0, UBT_BUFSIZ_CMD, USBD_FORCE_SHORT_XFER,
	    0, &sc->sc_cmd_xfer);
	if (error)
		goto bad;
	sc->sc_cmd_buf = usbd_get_buffer(sc->sc_cmd_xfer);
	sc->sc_cmd_busy = 0;

	/* ACL read */
	err = usbd_open_pipe(sc->sc_iface0, sc->sc_aclrd_addr,
				USBD_EXCLUSIVE_USE, &sc->sc_aclrd_pipe);
	if (err != USBD_NORMAL_COMPLETION) {
		error = EIO;
		goto bad;
	}
	error = usbd_create_xfer(sc->sc_aclrd_pipe, UBT_BUFSIZ_ACL,
	    0, 0, &sc->sc_aclrd_xfer);
	if (error)
		goto bad;
	sc->sc_aclrd_buf = usbd_get_buffer(sc->sc_aclrd_xfer);
	sc->sc_aclrd_busy = 0;
	ubt_recv_acl_start(sc);

	/* ACL write */
	err = usbd_open_pipe(sc->sc_iface0, sc->sc_aclwr_addr,
				USBD_EXCLUSIVE_USE, &sc->sc_aclwr_pipe);
	if (err != USBD_NORMAL_COMPLETION) {
		error = EIO;
		goto bad;
	}
	error = usbd_create_xfer(sc->sc_aclwr_pipe, UBT_BUFSIZ_ACL,
	    USBD_FORCE_SHORT_XFER, 0, &sc->sc_aclwr_xfer);
	if (error)
		goto bad;
	sc->sc_aclwr_buf = usbd_get_buffer(sc->sc_aclwr_xfer);
	sc->sc_aclwr_busy = 0;

	/* SCO read */
	if (sc->sc_scord_size > 0) {
		err = usbd_open_pipe(sc->sc_iface1, sc->sc_scord_addr,
					USBD_EXCLUSIVE_USE, &sc->sc_scord_pipe);
		if (err != USBD_NORMAL_COMPLETION) {
			error = EIO;
			goto bad;
		}

		for (i = 0 ; i < UBT_NXFERS ; i++) {
		        error = usbd_create_xfer(sc->sc_scord_pipe,
			    sc->sc_scord_size * UBT_NFRAMES,
			    0, UBT_NFRAMES,
			    &sc->sc_scord[i].xfer);
			if (error)
				goto bad;

			sc->sc_scord[i].buf =
			    usbd_get_buffer(sc->sc_scord[i].xfer);
			sc->sc_scord[i].softc = sc;
			sc->sc_scord[i].busy = 0;
			ubt_recv_sco_start1(sc, &sc->sc_scord[i]);
		}
	}

	/* SCO write */
	if (sc->sc_scowr_size > 0) {
		err = usbd_open_pipe(sc->sc_iface1, sc->sc_scowr_addr,
					USBD_EXCLUSIVE_USE, &sc->sc_scowr_pipe);
		if (err != USBD_NORMAL_COMPLETION) {
			error = EIO;
			goto bad;
		}

		for (i = 0 ; i < UBT_NXFERS ; i++) {
			error = usbd_create_xfer(sc->sc_scowr_pipe,
			    sc->sc_scowr_size * UBT_NFRAMES,
			    USBD_FORCE_SHORT_XFER, UBT_NFRAMES,
			    &sc->sc_scowr[i].xfer);
			if (error)
				goto bad;
			sc->sc_scowr[i].buf =
			    usbd_get_buffer(sc->sc_scowr[i].xfer);
			sc->sc_scowr[i].softc = sc;
			sc->sc_scowr[i].busy = 0;
		}

		sc->sc_scowr_busy = 0;
	}

	return 0;

bad:
	return error;
}

static void
ubt_disable(device_t self)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	DPRINTFN(1, "sc=%p\n", sc);

	if (sc->sc_enabled == 0)
		return;

	s = splusb();
	ubt_abortdealloc(sc);

	sc->sc_enabled = 0;
	sc->sc_parked = 0;
	splx(s);
}

static void
ubt_xmit_cmd(device_t self, struct mbuf *m)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	KASSERT(sc->sc_enabled);

	s = splusb();
	MBUFQ_ENQUEUE(&sc->sc_cmd_queue, m);

	if (sc->sc_cmd_busy == 0)
		ubt_xmit_cmd_start(sc);

	splx(s);
}

static void
ubt_xmit_cmd_start(struct ubt_softc *sc)
{
	usb_device_request_t req;
	usbd_status status;
	struct mbuf *m;
	int len;

	if (sc->sc_dying)
		return;

	if (MBUFQ_FIRST(&sc->sc_cmd_queue) == NULL)
		return;

	MBUFQ_DEQUEUE(&sc->sc_cmd_queue, m);
	KASSERT(m != NULL);

	DPRINTFN(15, "%s: xmit CMD packet (%d bytes)\n",
			device_xname(sc->sc_dev), m->m_pkthdr.len);

	sc->sc_refcnt++;
	sc->sc_cmd_busy = 1;

	len = m->m_pkthdr.len - 1;
	m_copydata(m, 1, len, sc->sc_cmd_buf);
	m_freem(m);

	memset(&req, 0, sizeof(req));
	req.bmRequestType = UT_WRITE_CLASS_DEVICE;
	USETW(req.wLength, len);

	usbd_setup_default_xfer(sc->sc_cmd_xfer,
				sc->sc_udev,
				sc,
				UBT_CMD_TIMEOUT,
				&req,
				sc->sc_cmd_buf,
				len,
				USBD_FORCE_SHORT_XFER,
				ubt_xmit_cmd_complete);

	status = usbd_transfer(sc->sc_cmd_xfer);

	KASSERT(status != USBD_NORMAL_COMPLETION);

	if (status != USBD_IN_PROGRESS) {
		DPRINTF("usbd_transfer status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_refcnt--;
		sc->sc_cmd_busy = 0;
	}
}

static void
ubt_xmit_cmd_complete(struct usbd_xfer *xfer,
			void * h, usbd_status status)
{
	struct ubt_softc *sc = h;
	uint32_t count;

	DPRINTFN(15, "%s: CMD complete status=%s (%d)\n",
			device_xname(sc->sc_dev), usbd_errstr(status), status);

	sc->sc_cmd_busy = 0;

	if (--sc->sc_refcnt < 0) {
		DPRINTF("sc_refcnt=%d\n", sc->sc_refcnt);
		usb_detach_wakeupold(sc->sc_dev);
		return;
	}

	if (sc->sc_dying) {
		DPRINTF("sc_dying\n");
		return;
	}

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF("status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_stats.err_tx++;
		return;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &count, NULL);
	sc->sc_stats.cmd_tx++;
	sc->sc_stats.byte_tx += count;

	ubt_xmit_cmd_start(sc);
}

static void
ubt_xmit_acl(device_t self, struct mbuf *m)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	KASSERT(sc->sc_enabled);

	s = splusb();
	MBUFQ_ENQUEUE(&sc->sc_aclwr_queue, m);

	if (sc->sc_aclwr_busy == 0)
		ubt_xmit_acl_start(sc);

	splx(s);
}

static void
ubt_xmit_acl_start(struct ubt_softc *sc)
{
	struct mbuf *m;
	usbd_status status;
	int len;

	if (sc->sc_dying)
		return;

	if (MBUFQ_FIRST(&sc->sc_aclwr_queue) == NULL)
		return;

	sc->sc_refcnt++;
	sc->sc_aclwr_busy = 1;

	MBUFQ_DEQUEUE(&sc->sc_aclwr_queue, m);
	KASSERT(m != NULL);

	DPRINTFN(15, "%s: xmit ACL packet (%d bytes)\n",
			device_xname(sc->sc_dev), m->m_pkthdr.len);

	len = m->m_pkthdr.len - 1;
	if (len > UBT_BUFSIZ_ACL) {
		DPRINTF("%s: truncating ACL packet (%d => %d)!\n",
			device_xname(sc->sc_dev), len, UBT_BUFSIZ_ACL);

		len = UBT_BUFSIZ_ACL;
	}

	m_copydata(m, 1, len, sc->sc_aclwr_buf);
	m_freem(m);

	sc->sc_stats.acl_tx++;
	sc->sc_stats.byte_tx += len;

	usbd_setup_xfer(sc->sc_aclwr_xfer,
			sc,
			sc->sc_aclwr_buf,
			len,
			USBD_FORCE_SHORT_XFER,
			UBT_ACL_TIMEOUT,
			ubt_xmit_acl_complete);

	status = usbd_transfer(sc->sc_aclwr_xfer);

	KASSERT(status != USBD_NORMAL_COMPLETION);

	if (status != USBD_IN_PROGRESS) {
		DPRINTF("usbd_transfer status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_refcnt--;
		sc->sc_aclwr_busy = 0;
	}
}

static void
ubt_xmit_acl_complete(struct usbd_xfer *xfer,
		void * h, usbd_status status)
{
	struct ubt_softc *sc = h;

	DPRINTFN(15, "%s: ACL complete status=%s (%d)\n",
		device_xname(sc->sc_dev), usbd_errstr(status), status);

	sc->sc_aclwr_busy = 0;

	if (--sc->sc_refcnt < 0) {
		usb_detach_wakeupold(sc->sc_dev);
		return;
	}

	if (sc->sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF("status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_stats.err_tx++;

		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_aclwr_pipe);
		else
			return;
	}

	ubt_xmit_acl_start(sc);
}

static void
ubt_xmit_sco(device_t self, struct mbuf *m)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	KASSERT(sc->sc_enabled);

	s = splusb();
	MBUFQ_ENQUEUE(&sc->sc_scowr_queue, m);

	if (sc->sc_scowr_busy == 0)
		ubt_xmit_sco_start(sc);

	splx(s);
}

static void
ubt_xmit_sco_start(struct ubt_softc *sc)
{
	int i;

	if (sc->sc_dying || sc->sc_scowr_size == 0)
		return;

	for (i = 0 ; i < UBT_NXFERS ; i++) {
		if (sc->sc_scowr[i].busy)
			continue;

		ubt_xmit_sco_start1(sc, &sc->sc_scowr[i]);
	}
}

static void
ubt_xmit_sco_start1(struct ubt_softc *sc, struct ubt_isoc_xfer *isoc)
{
	struct mbuf *m;
	uint8_t *buf;
	int num, len, size, space;

	space = sc->sc_scowr_size * UBT_NFRAMES;
	buf = isoc->buf;
	len = 0;

	/*
	 * Fill the request buffer with data from the queue,
	 * keeping any leftover packet on our private hook.
	 *
	 * Complete packets are passed back up to the stack
	 * for disposal, since we can't rely on the controller
	 * to tell us when it has finished with them.
	 */

	m = sc->sc_scowr_mbuf;
	while (space > 0) {
		if (m == NULL) {
			MBUFQ_DEQUEUE(&sc->sc_scowr_queue, m);
			if (m == NULL)
				break;

			m_adj(m, 1);	/* packet type */
		}

		if (m->m_pkthdr.len > 0) {
			size = MIN(m->m_pkthdr.len, space);

			m_copydata(m, 0, size, buf);
			m_adj(m, size);

			buf += size;
			len += size;
			space -= size;
		}

		if (m->m_pkthdr.len == 0) {
			sc->sc_stats.sco_tx++;
			if (!hci_complete_sco(sc->sc_unit, m))
				sc->sc_stats.err_tx++;

			m = NULL;
		}
	}
	sc->sc_scowr_mbuf = m;

	DPRINTFN(15, "isoc=%p, len=%d, space=%d\n", isoc, len, space);

	if (len == 0)	/* nothing to send */
		return;

	sc->sc_refcnt++;
	sc->sc_scowr_busy = 1;
	sc->sc_stats.byte_tx += len;
	isoc->busy = 1;

	/*
	 * calculate number of isoc frames and sizes
	 */

	for (num = 0 ; len > 0 ; num++) {
		size = MIN(sc->sc_scowr_size, len);

		isoc->size[num] = size;
		len -= size;
	}

	usbd_setup_isoc_xfer(isoc->xfer,
			     isoc,
			     isoc->size,
			     num,
			     USBD_FORCE_SHORT_XFER,
			     ubt_xmit_sco_complete);

	usbd_transfer(isoc->xfer);
}

static void
ubt_xmit_sco_complete(struct usbd_xfer *xfer,
		void * h, usbd_status status)
{
	struct ubt_isoc_xfer *isoc = h;
	struct ubt_softc *sc;
	int i;

	KASSERT(xfer == isoc->xfer);
	sc = isoc->softc;

	DPRINTFN(15, "isoc=%p, status=%s (%d)\n",
		isoc, usbd_errstr(status), status);

	isoc->busy = 0;

	for (i = 0 ; ; i++) {
		if (i == UBT_NXFERS) {
			sc->sc_scowr_busy = 0;
			break;
		}

		if (sc->sc_scowr[i].busy)
			break;
	}

	if (--sc->sc_refcnt < 0) {
		usb_detach_wakeupold(sc->sc_dev);
		return;
	}

	if (sc->sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF("status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_stats.err_tx++;

		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_scowr_pipe);
		else
			return;
	}

	ubt_xmit_sco_start(sc);
}

/*
 * load incoming data into an mbuf with
 * leading type byte
 */
static struct mbuf *
ubt_mbufload(uint8_t *buf, int count, uint8_t type)
{
	struct mbuf *m;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return NULL;

	*mtod(m, uint8_t *) = type;
	m->m_pkthdr.len = m->m_len = MHLEN;
	m_copyback(m, 1, count, buf);	// (extends if necessary)
	if (m->m_pkthdr.len != MAX(MHLEN, count + 1)) {
		m_freem(m);
		return NULL;
	}

	m->m_pkthdr.len = count + 1;
	m->m_len = MIN(MHLEN, m->m_pkthdr.len);

	return m;
}

static void
ubt_recv_event(struct usbd_xfer *xfer, void * h, usbd_status status)
{
	struct ubt_softc *sc = h;
	struct mbuf *m;
	uint32_t count;
	void *buf;

	DPRINTFN(15, "sc=%p status=%s (%d)\n",
		    sc, usbd_errstr(status), status);

	if (status != USBD_NORMAL_COMPLETION || sc->sc_dying)
		return;

	usbd_get_xfer_status(xfer, NULL, &buf, &count, NULL);

	if (sc->sc_intel_loading) {
		ubt_intel_enqueue(sc->sc_intel_ui, buf, count);
		return;
	}

	if (count < sizeof(hci_event_hdr_t) - 1) {
		DPRINTF("dumped undersized event (count = %d)\n", count);
		sc->sc_stats.err_rx++;
		return;
	}

	sc->sc_stats.evt_rx++;
	sc->sc_stats.byte_rx += count;

	m = ubt_mbufload(buf, count, HCI_EVENT_PKT);
	if (m == NULL || !hci_input_event(sc->sc_unit, m))
		sc->sc_stats.err_rx++;
}

static void
ubt_recv_acl_start(struct ubt_softc *sc)
{
	usbd_status status;

	DPRINTFN(15, "sc=%p\n", sc);

	if (sc->sc_aclrd_busy || sc->sc_dying) {
		DPRINTF("sc_aclrd_busy=%d, sc_dying=%d\n",
			sc->sc_aclrd_busy,
			sc->sc_dying);

		return;
	}

	sc->sc_refcnt++;
	sc->sc_aclrd_busy = 1;

	usbd_setup_xfer(sc->sc_aclrd_xfer,
			sc,
			sc->sc_aclrd_buf,
			UBT_BUFSIZ_ACL,
			USBD_SHORT_XFER_OK,
			USBD_NO_TIMEOUT,
			ubt_recv_acl_complete);

	status = usbd_transfer(sc->sc_aclrd_xfer);

	KASSERT(status != USBD_NORMAL_COMPLETION);

	if (status != USBD_IN_PROGRESS) {
		DPRINTF("usbd_transfer status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_refcnt--;
		sc->sc_aclrd_busy = 0;
	}
}

static void
ubt_recv_acl_complete(struct usbd_xfer *xfer,
		void * h, usbd_status status)
{
	struct ubt_softc *sc = h;
	struct mbuf *m;
	uint32_t count;
	void *buf;

	DPRINTFN(15, "sc=%p status=%s (%d)\n",
			sc, usbd_errstr(status), status);

	sc->sc_aclrd_busy = 0;

	if (--sc->sc_refcnt < 0) {
		DPRINTF("refcnt = %d\n", sc->sc_refcnt);
		usb_detach_wakeupold(sc->sc_dev);
		return;
	}

	if (sc->sc_dying) {
		DPRINTF("sc_dying\n");
		return;
	}

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF("status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_stats.err_rx++;

		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_aclrd_pipe);
		else
			return;
	} else {
		usbd_get_xfer_status(xfer, NULL, &buf, &count, NULL);

		if (sc->sc_intel_loading) {
			/* The bootloader answers on this endpoint too. */
			ubt_intel_enqueue(sc->sc_intel_ui, buf, count);
		} else if (count < sizeof(hci_acldata_hdr_t) - 1) {
			DPRINTF("dumped undersized packet (%d)\n", count);
			sc->sc_stats.err_rx++;
		} else {
			sc->sc_stats.acl_rx++;
			sc->sc_stats.byte_rx += count;

			m = ubt_mbufload(buf, count, HCI_ACL_DATA_PKT);
			if (m == NULL || !hci_input_acl(sc->sc_unit, m))
				sc->sc_stats.err_rx++;
		}
	}

	/* and restart */
	ubt_recv_acl_start(sc);
}

static void
ubt_recv_sco_start1(struct ubt_softc *sc, struct ubt_isoc_xfer *isoc)
{
	int i;

	DPRINTFN(15, "sc=%p, isoc=%p\n", sc, isoc);

	if (isoc->busy || sc->sc_dying || sc->sc_scord_size == 0) {
		DPRINTF("%s%s%s\n",
			isoc->busy ? " busy" : "",
			sc->sc_dying ? " dying" : "",
			sc->sc_scord_size == 0 ? " size=0" : "");

		return;
	}

	sc->sc_refcnt++;
	isoc->busy = 1;

	for (i = 0 ; i < UBT_NFRAMES ; i++)
		isoc->size[i] = sc->sc_scord_size;

	usbd_setup_isoc_xfer(isoc->xfer,
			     isoc,
			     isoc->size,
			     UBT_NFRAMES,
			     USBD_SHORT_XFER_OK,
			     ubt_recv_sco_complete);

	usbd_transfer(isoc->xfer);
}

static void
ubt_recv_sco_complete(struct usbd_xfer *xfer,
		void * h, usbd_status status)
{
	struct ubt_isoc_xfer *isoc = h;
	struct ubt_softc *sc;
	struct mbuf *m;
	uint32_t count;
	uint8_t *ptr, *frame;
	int i, size, got, want;

	KASSERT(isoc != NULL);
	KASSERT(isoc->xfer == xfer);

	sc = isoc->softc;
	isoc->busy = 0;

	if (--sc->sc_refcnt < 0) {
		DPRINTF("refcnt=%d\n", sc->sc_refcnt);
		usb_detach_wakeupold(sc->sc_dev);
		return;
	}

	if (sc->sc_dying) {
		DPRINTF("sc_dying\n");
		return;
	}

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF("status=%s (%d)\n",
			usbd_errstr(status), status);

		sc->sc_stats.err_rx++;

		if (status == USBD_STALLED) {
			usbd_clear_endpoint_stall_async(sc->sc_scord_pipe);
			goto restart;
		}

		return;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &count, NULL);
	if (count == 0)
		goto restart;

	DPRINTFN(15, "sc=%p, isoc=%p, count=%u\n",
			sc, isoc, count);

	sc->sc_stats.byte_rx += count;

	/*
	 * Extract SCO packets from ISOC frames. The way we have it,
	 * no SCO packet can be bigger than MHLEN. This is unlikely
	 * to actually happen, but if we ran out of mbufs and lost
	 * sync then we may get spurious data that makes it seem that
	 * way, so we discard data that wont fit. This doesnt really
	 * help with the lost sync situation alas.
	 */

	m = sc->sc_scord_mbuf;
	if (m != NULL) {
		sc->sc_scord_mbuf = NULL;
		ptr = mtod(m, uint8_t *) + m->m_pkthdr.len;
		got = m->m_pkthdr.len;
		want = sizeof(hci_scodata_hdr_t);
		if (got >= want)
			want += mtod(m, hci_scodata_hdr_t *)->length ;
	} else {
		ptr = NULL;
		got = 0;
		want = 0;
	}

	for (i = 0 ; i < UBT_NFRAMES ; i++) {
		frame = isoc->buf + (i * sc->sc_scord_size);

		while (isoc->size[i] > 0) {
			size = isoc->size[i];

			if (m == NULL) {
				MGETHDR(m, M_DONTWAIT, MT_DATA);
				if (m == NULL) {
					aprint_error_dev(sc->sc_dev,
					    "out of memory (xfer halted)\n");

					sc->sc_stats.err_rx++;
					return;		/* lost sync */
				}

				ptr = mtod(m, uint8_t *);
				*ptr++ = HCI_SCO_DATA_PKT;
				got = 1;
				want = sizeof(hci_scodata_hdr_t);
			}

			if (got + size > want)
				size = want - got;

			memcpy(ptr, frame, size);

			ptr += size;
			got += size;
			frame += size;

			if (got == want) {
				/*
				 * If we only got a header, add the packet
				 * length to our want count. Send complete
				 * packets up to protocol stack.
				 */
				if (want == sizeof(hci_scodata_hdr_t)) {
					uint32_t len =
					    mtod(m, hci_scodata_hdr_t *)->length;
					want += len;
					if (len == 0 || want > MHLEN) {
						aprint_error_dev(sc->sc_dev,
						    "packet too large %u "
						    "(lost sync)\n", len);
						sc->sc_stats.err_rx++;
						return;
					}
				}

				if (got == want) {
					m->m_pkthdr.len = m->m_len = got;
					sc->sc_stats.sco_rx++;
					if (!hci_input_sco(sc->sc_unit, m))
						sc->sc_stats.err_rx++;

					m = NULL;
				}
			}

			isoc->size[i] -= size;
		}
	}

	if (m != NULL) {
		m->m_pkthdr.len = m->m_len = got;
		sc->sc_scord_mbuf = m;
	}

restart: /* and restart */
	ubt_recv_sco_start1(sc, isoc);
}

void
ubt_stats(device_t self, struct bt_stats *dest, int flush)
{
	struct ubt_softc *sc = device_private(self);
	int s;

	s = splusb();
	memcpy(dest, &sc->sc_stats, sizeof(struct bt_stats));

	if (flush)
		memset(&sc->sc_stats, 0, sizeof(struct bt_stats));

	splx(s);
}
