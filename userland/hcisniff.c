/*
 * hcisniff [device]: print every HCI packet passing a unit (commands,
 * events, ACL, SCO) with direction, decoded headers and a hex dump.
 * Needs root (the kernel's policy hides ACL data and some events from
 * unprivileged sockets).  Built: cc -o hcisniff hcisniff.c -lbluetooth
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <bluetooth.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "ubt0";
	struct bt_devfilter f;
	uint8_t buf[2048];
	char cbuf[64];
	int s, on = 1, e;

	s = bt_devopen(dev, 0);
	if (s < 0)
		err(1, "bt_devopen %s", dev);
	memset(&f, 0, sizeof(f));
	bt_devfilter_pkt_set(&f, HCI_CMD_PKT);
	bt_devfilter_pkt_set(&f, HCI_EVENT_PKT);
	bt_devfilter_pkt_set(&f, HCI_ACL_DATA_PKT);
	bt_devfilter_pkt_set(&f, HCI_SCO_DATA_PKT);
	for (e = 1; e <= 0xff; e++)
		bt_devfilter_evt_set(&f, e);
	if (bt_devfilter(s, &f, NULL) < 0)
		err(1, "bt_devfilter");
	if (setsockopt(s, BTPROTO_HCI, SO_HCI_DIRECTION, &on, sizeof(on)) < 0)
		warn("SO_HCI_DIRECTION");

	for (;;) {
		struct iovec iov = { buf, sizeof(buf) };
		struct msghdr msg;
		struct cmsghdr *c;
		struct timeval tv;
		ssize_t n;
		int dir = -1, i, max;

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		n = recvmsg(s, &msg, 0);
		if (n <= 0)
			break;
		for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
			if (c->cmsg_level == BTPROTO_HCI &&
			    c->cmsg_type == SCM_HCI_DIRECTION)
				memcpy(&dir, CMSG_DATA(c), sizeof(dir));
		gettimeofday(&tv, NULL);
		printf("%02d:%02d:%02d.%03d %s ",
		    (int)(tv.tv_sec / 3600 % 24), (int)(tv.tv_sec / 60 % 60),
		    (int)(tv.tv_sec % 60), (int)(tv.tv_usec / 1000),
		    dir == 0 ? "OUT" : dir == 1 ? "IN " : "?? ");
		switch (buf[0]) {
		case HCI_CMD_PKT:
			printf("CMD  opcode %#06x plen %u", buf[1] | (buf[2] << 8),
			    buf[3]);
			break;
		case HCI_EVENT_PKT:
			printf("EVT  %#04x plen %u", buf[1], buf[2]);
			if (buf[1] == 0x0e && n >= 6)
				printf(" (complete opcode %#06x status %#x)",
				    buf[4] | (buf[5] << 8), n >= 7 ? buf[6] : 0xff);
			if (buf[1] == 0x0f && n >= 7)
				printf(" (status %#x opcode %#06x)", buf[3],
				    buf[5] | (buf[6] << 8));
			if (buf[1] == 0x03 && n >= 5)
				printf(" (conn complete status %#x)", buf[3]);
			break;
		case HCI_ACL_DATA_PKT:
			printf("ACL  handle %#x flags %#x len %u",
			    (buf[1] | (buf[2] << 8)) & 0xfff, buf[2] >> 4,
			    buf[3] | (buf[4] << 8));
			if (n >= 9)
				printf(" l2cap len %u cid %#x",
				    buf[5] | (buf[6] << 8), buf[7] | (buf[8] << 8));
			if (n >= 13 && (buf[7] | (buf[8] << 8)) == 1)
				printf(" sig code %#x id %u", buf[9], buf[10]);
			break;
		case HCI_SCO_DATA_PKT:
			printf("SCO  len %u", buf[3]);
			break;
		default:
			printf("type %#x", buf[0]);
		}
		printf("\n   ");
		max = n > 64 ? 64 : (int)n;
		for (i = 0; i < max; i++)
			printf(" %02x", buf[i]);
		printf("%s\n", n > 64 ? " ..." : "");
		fflush(stdout);
	}
	return 0;
}
