/*	$NetBSD: hci.c,v 1.4 2024/09/07 13:57:25 mlelstv Exp $	*/

/*-
 * Copyright (c) 2006 Itronix Inc.
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
 * Copyright (c) 2001-2002 Maksim Yevmenkin <m_evmenkin@yahoo.com>
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__RCSID("$NetBSD: hci.c,v 1.4 2024/09/07 13:57:25 mlelstv Exp $");

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/endian.h>
#include <bluetooth.h>
#include <errno.h>
#include <event.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "bthcid.h"

static struct event	hci_ev;

static void process_hci
		(int, short, void *);

static int process_pin_code_request_event
		(int, struct sockaddr_bt *, bdaddr_t *);
static int process_link_key_request_event
		(int, struct sockaddr_bt *, bdaddr_t *);
static int process_link_key_notification_event
		(int, struct sockaddr_bt *, hci_link_key_notification_ep *);

static int send_link_key_reply
		(int, struct sockaddr_bt *, bdaddr_t *, uint8_t *);
static int send_hci_cmd
		(int, struct sockaddr_bt *, uint16_t, size_t, void *);

/*
 * LISPBSD: Secure Simple Pairing, "just works" (no display, no keyboard).
 * The kernel's hci.h has the IO capability reply opcode wrong (0x042a is
 * Reject SCO Connection Request), so the opcodes are spelled out here.
 */
#define SSP_CMD_IO_CAPABILITY_REP	0x042b
#define SSP_CMD_USER_CONFIRM_REP	0x042c
#define SSP_CMD_USER_PASSKEY_NEG_REP	0x042f
#define SSP_IO_CAP_NO_INPUT_NO_OUTPUT	0x03
#define SSP_AUTH_GENERAL_BONDING	0x04	/* MITM not required */

static int process_io_capability_request_event(int, struct sockaddr_bt *,
    hci_io_capability_req_ep *);
static int process_user_confirm_request_event(int, struct sockaddr_bt *,
    hci_user_confirm_req_ep *);
static int process_user_passkey_request_event(int, struct sockaddr_bt *,
    hci_user_passkey_req_ep *);
static int process_simple_pairing_complete_event(int, struct sockaddr_bt *,
    hci_simple_pairing_compl_ep *);
static void enable_ssp(int, const char *);

static char dev_name[HCI_DEVNAME_SIZE];

/* Initialise HCI Events */
int
init_hci(const char *device)
{
	struct bt_devfilter	filter;
	int			hci;

	hci = bt_devopen(device, 0);
	if (hci < 0)
		return -1;

	memset(&filter, 0, sizeof(filter));
	bt_devfilter_pkt_set(&filter, HCI_EVENT_PKT);
	bt_devfilter_evt_set(&filter, HCI_EVENT_PIN_CODE_REQ);
	bt_devfilter_evt_set(&filter, HCI_EVENT_LINK_KEY_REQ);
	bt_devfilter_evt_set(&filter, HCI_EVENT_LINK_KEY_NOTIFICATION);
	bt_devfilter_evt_set(&filter, HCI_EVENT_IO_CAPABILITY_REQ);
	bt_devfilter_evt_set(&filter, HCI_EVENT_USER_CONFIRM_REQ);
	bt_devfilter_evt_set(&filter, HCI_EVENT_USER_PASSKEY_REQ);
	bt_devfilter_evt_set(&filter, HCI_EVENT_SIMPLE_PAIRING_COMPL);
	if (bt_devfilter(hci, &filter, NULL) < 0) {
		close(hci);
		return -1;
	}

	/*
	 * LISPBSD: with a device named, switch Secure Simple Pairing on in
	 * its controller and unmask the SSP events (the kernel of 149 and
	 * later does this itself at init; a reset by btconfig down/up undoes
	 * it either way, restart bthcid after one).
	 */
	if (device != NULL)
		enable_ssp(hci, device);

	event_set(&hci_ev, hci, EV_READ | EV_PERSIST, process_hci, NULL);
	if (event_add(&hci_ev, NULL) < 0) {
		close(hci);
		return -1;
	}

	return 0;
}

/* Process an HCI event */
static void
process_hci(int sock, short ev, void *arg)
{
	char			 buffer[HCI_EVENT_PKT_SIZE];
	hci_event_hdr_t		*event = (hci_event_hdr_t *)buffer;
	struct sockaddr_bt	 addr;
	int			 n;
	socklen_t		 size;

	size = sizeof(addr);
	n = recvfrom(sock, buffer, sizeof(buffer), 0,
			(struct sockaddr *) &addr, &size);
	if (n < 0) {
		syslog(LOG_ERR, "Could not receive from HCI socket: %m");
		return;
	}

	if (event->type != HCI_EVENT_PKT) {
		syslog(LOG_ERR, "Received unexpected HCI packet, "
				"type=%#x", event->type);

		return;
	}

	if (!bt_devname(dev_name, &addr.bt_bdaddr))
		strlcpy(dev_name, "unknown", sizeof(dev_name));

	switch (event->event) {
	case HCI_EVENT_PIN_CODE_REQ:
		process_pin_code_request_event(sock, &addr,
					    (bdaddr_t *)(event + 1));
		break;

	case HCI_EVENT_LINK_KEY_REQ:
		process_link_key_request_event(sock, &addr,
					    (bdaddr_t *)(event + 1));
		break;

	case HCI_EVENT_LINK_KEY_NOTIFICATION:
		process_link_key_notification_event(sock, &addr,
			(hci_link_key_notification_ep *)(event + 1));
		break;

	case HCI_EVENT_IO_CAPABILITY_REQ:
		process_io_capability_request_event(sock, &addr,
			(hci_io_capability_req_ep *)(event + 1));
		break;

	case HCI_EVENT_USER_CONFIRM_REQ:
		process_user_confirm_request_event(sock, &addr,
			(hci_user_confirm_req_ep *)(event + 1));
		break;

	case HCI_EVENT_USER_PASSKEY_REQ:
		process_user_passkey_request_event(sock, &addr,
			(hci_user_passkey_req_ep *)(event + 1));
		break;

	case HCI_EVENT_SIMPLE_PAIRING_COMPL:
		process_simple_pairing_complete_event(sock, &addr,
			(hci_simple_pairing_compl_ep *)(event + 1));
		break;

	default:
		syslog(LOG_ERR, "Received unexpected HCI event, "
				"event=%#x", event->event);
		break;
	}

	return;
}

/* Process PIN_Code_Request event */
static int
process_pin_code_request_event(int sock, struct sockaddr_bt *addr,
		bdaddr_t *bdaddr)
{
	uint8_t	*pin;

	syslog(LOG_DEBUG, "Got PIN_Code_Request event from %s, "
			  "remote bdaddr %s",
			  dev_name,
			  bt_ntoa(bdaddr, NULL));

	pin = lookup_pin(&addr->bt_bdaddr, bdaddr);
	if (pin != NULL)
		return send_pin_code_reply(sock, addr, bdaddr, pin);

	if (send_client_request(&addr->bt_bdaddr, bdaddr, sock) == 0)
		return send_pin_code_reply(sock, addr, bdaddr, NULL);

	return 0;
}

/* Process Link_Key_Request event */
static int
process_link_key_request_event(int sock, struct sockaddr_bt *addr,
		bdaddr_t *bdaddr)
{
	uint8_t		*key;

	syslog(LOG_DEBUG,
		"Got Link_Key_Request event from %s, remote bdaddr %s",
		dev_name, bt_ntoa(bdaddr, NULL));

	key = lookup_key(&addr->bt_bdaddr, bdaddr);

	if (key != NULL) {
		syslog(LOG_DEBUG, "Found Key, remote bdaddr %s",
				bt_ntoa(bdaddr, NULL));

		return send_link_key_reply(sock, addr, bdaddr, key);
	}

	syslog(LOG_DEBUG, "Could not find link key for remote bdaddr %s",
			bt_ntoa(bdaddr, NULL));

	return send_link_key_reply(sock, addr, bdaddr, NULL);
}

/* Send PIN_Code_[Negative]_Reply */
int
send_pin_code_reply(int sock, struct sockaddr_bt *addr,
	bdaddr_t *bdaddr, uint8_t *pin)
{
	int	n;

	if (pin != NULL) {
		hci_pin_code_rep_cp	 cp;

		syslog(LOG_DEBUG, "Sending PIN_Code_Reply to %s "
				  "for remote bdaddr %s",
				  dev_name,
				  bt_ntoa(bdaddr, NULL));

		bdaddr_copy(&cp.bdaddr, bdaddr);
		memcpy(cp.pin, pin, HCI_PIN_SIZE);

		n = HCI_PIN_SIZE;
		while (n > 0 && pin[n - 1] == 0)
			n--;
		cp.pin_size = n;

		n = send_hci_cmd(sock, addr,
				HCI_CMD_PIN_CODE_REP, sizeof(cp), &cp);

	} else {
		syslog(LOG_DEBUG, "Sending PIN_Code_Negative_Reply to %s "
				  "for remote bdaddr %s",
				  dev_name,
				  bt_ntoa(bdaddr, NULL));

		n = send_hci_cmd(sock, addr, HCI_CMD_PIN_CODE_NEG_REP,
					sizeof(bdaddr_t), bdaddr);
	}

	if (n < 0) {
		syslog(LOG_ERR, "Could not send PIN code reply to %s "
				"for remote bdaddr %s: %m",
				dev_name,
				bt_ntoa(bdaddr, NULL));

		return -1;
	}

	return 0;
}

/* Send Link_Key_[Negative]_Reply */
static int
send_link_key_reply(int sock, struct sockaddr_bt *addr,
		bdaddr_t *bdaddr, uint8_t *key)
{
	int	n;

	if (key != NULL) {
		hci_link_key_rep_cp	cp;

		bdaddr_copy(&cp.bdaddr, bdaddr);
		memcpy(&cp.key, key, sizeof(cp.key));

		syslog(LOG_DEBUG, "Sending Link_Key_Reply to %s "
				"for remote bdaddr %s",
				dev_name, bt_ntoa(bdaddr, NULL));

		n = send_hci_cmd(sock, addr, HCI_CMD_LINK_KEY_REP, sizeof(cp), &cp);
	} else {
		hci_link_key_neg_rep_cp	cp;

		bdaddr_copy(&cp.bdaddr, bdaddr);

		syslog(LOG_DEBUG, "Sending Link_Key_Negative_Reply to %s "
				"for remote bdaddr %s",
				dev_name, bt_ntoa(bdaddr, NULL));

		n = send_hci_cmd(sock, addr, HCI_CMD_LINK_KEY_NEG_REP, sizeof(cp), &cp);
	}

	if (n < 0) {
		syslog(LOG_ERR, "Could not send link key reply to %s "
				"for remote bdaddr %s: %m",
				dev_name, bt_ntoa(bdaddr, NULL));
		return -1;
	}

	return 0;
}

/* Process Link_Key_Notification event */
static int
process_link_key_notification_event(int sock, struct sockaddr_bt *addr,
		hci_link_key_notification_ep *ep)
{

	syslog(LOG_DEBUG, "Got Link_Key_Notification event from %s, "
			"remote bdaddr %s",
			dev_name,
			bt_ntoa(&ep->bdaddr, NULL));

	save_key(&addr->bt_bdaddr, &ep->bdaddr, ep->key);
	return 0;
}

/* Process IO_Capability_Request event: we have no display and no keyboard */
static int
process_io_capability_request_event(int sock, struct sockaddr_bt *addr,
		hci_io_capability_req_ep *ep)
{
	hci_io_capability_rep_cp cp;

	syslog(LOG_DEBUG, "Got IO_Capability_Request event from %s, "
			"remote bdaddr %s", dev_name, bt_ntoa(&ep->bdaddr, NULL));

	memset(&cp, 0, sizeof(cp));
	bdaddr_copy(&cp.bdaddr, &ep->bdaddr);
	cp.io_cap = SSP_IO_CAP_NO_INPUT_NO_OUTPUT;
	cp.oob_data = 0;
	cp.auth_req = SSP_AUTH_GENERAL_BONDING;

	if (send_hci_cmd(sock, addr, SSP_CMD_IO_CAPABILITY_REP, sizeof(cp),
	    &cp) < 0) {
		syslog(LOG_ERR, "Could not send IO capability reply to %s "
				"for remote bdaddr %s: %m",
				dev_name, bt_ntoa(&ep->bdaddr, NULL));
		return -1;
	}
	return 0;
}

/* Process User_Confirmation_Request event: nobody can compare the number, accept */
static int
process_user_confirm_request_event(int sock, struct sockaddr_bt *addr,
		hci_user_confirm_req_ep *ep)
{
	hci_user_confirm_rep_cp cp;

	syslog(LOG_DEBUG, "Got User_Confirmation_Request event from %s, "
			"remote bdaddr %s, value %06u, accepting",
			dev_name, bt_ntoa(&ep->bdaddr, NULL),
			le32toh(ep->value));

	bdaddr_copy(&cp.bdaddr, &ep->bdaddr);
	if (send_hci_cmd(sock, addr, SSP_CMD_USER_CONFIRM_REP, sizeof(cp),
	    &cp) < 0) {
		syslog(LOG_ERR, "Could not send user confirmation reply to %s "
				"for remote bdaddr %s: %m",
				dev_name, bt_ntoa(&ep->bdaddr, NULL));
		return -1;
	}
	return 0;
}

/* Process User_Passkey_Request event: no keyboard to type one */
static int
process_user_passkey_request_event(int sock, struct sockaddr_bt *addr,
		hci_user_passkey_req_ep *ep)
{
	bdaddr_t bdaddr;

	syslog(LOG_DEBUG, "Got User_Passkey_Request event from %s, "
			"remote bdaddr %s, rejecting",
			dev_name, bt_ntoa(&ep->bdaddr, NULL));

	bdaddr_copy(&bdaddr, &ep->bdaddr);
	return send_hci_cmd(sock, addr, SSP_CMD_USER_PASSKEY_NEG_REP,
	    sizeof(bdaddr), &bdaddr);
}

/* Process Simple_Pairing_Complete event */
static int
process_simple_pairing_complete_event(int sock, struct sockaddr_bt *addr,
		hci_simple_pairing_compl_ep *ep)
{

	syslog(LOG_DEBUG, "Simple pairing with %s %s (status %#x)",
			bt_ntoa(&ep->bdaddr, NULL),
			ep->status == 0 ? "complete" : "failed", ep->status);
	return 0;
}

/* Switch SSP on in the controller and unmask its events (Linux's mask) */
static void
enable_ssp(int sock, const char *device)
{
	static const uint8_t mask[HCI_EVENT_MASK_SIZE] =
	    { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	struct sockaddr_bt sa;
	hci_set_event_mask_cp em;
	uint8_t mode = 1;

	memset(&sa, 0, sizeof(sa));
	sa.bt_len = sizeof(sa);
	sa.bt_family = AF_BLUETOOTH;
	if (!bt_devaddr(device, &sa.bt_bdaddr)) {
		syslog(LOG_ERR, "enable_ssp: no address for %s: %m", device);
		return;
	}
	memcpy(em.event_mask, mask, sizeof(em.event_mask));
	if (send_hci_cmd(sock, &sa, HCI_CMD_SET_EVENT_MASK, sizeof(em), &em) < 0)
		syslog(LOG_ERR, "Could not set event mask on %s: %m", device);
	if (send_hci_cmd(sock, &sa, HCI_CMD_WRITE_SIMPLE_PAIRING_MODE,
	    sizeof(mode), &mode) < 0)
		syslog(LOG_ERR, "Could not enable simple pairing on %s: %m",
		    device);
	else
		syslog(LOG_DEBUG, "Simple pairing enabled on %s", device);
	/*
	 * Secure Connections host support (P-256 pairing, BT 4.1): peers in
	 * "secure connections only" mode refuse to authenticate without it.
	 * Not in hci.h; opcode 0x0c7a.  Refused by older controllers, which
	 * is harmless.
	 */
	if (send_hci_cmd(sock, &sa, 0x0c7a, sizeof(mode), &mode) < 0)
		syslog(LOG_ERR, "Could not enable secure connections on %s: %m",
		    device);
}

/* Send HCI Command Packet to socket */
static int
send_hci_cmd(int sock, struct sockaddr_bt *sa, uint16_t opcode, size_t len, void *buf)
{
	char msg[HCI_CMD_PKT_SIZE];
	hci_cmd_hdr_t *h = (hci_cmd_hdr_t *)msg;

	h->type = HCI_CMD_PKT;
	h->opcode = htole16(opcode);
	h->length = len;

	if (len > 0)
		memcpy(msg + sizeof(hci_cmd_hdr_t), buf, len);

	return sendto(sock, msg, sizeof(hci_cmd_hdr_t) + len, MSG_NOSIGNAL,
			(struct sockaddr *)sa, sizeof(*sa));
}
