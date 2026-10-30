/*
 * Test harness for bta2dpd's AVDTP suspend/start handling (docs/power-findings-2026-10-30.org).
 * Build next to the sources:
 *   cc -O1 -g -I. -o bttest bttest.c avdtp.c sbc_encode.c -lbluetooth -levent -lutil
 * (sbc_coeffs.h and sbc_crc.h come from a normal make first).
 */
#define main bta2dpd_main
#include "bta2dpd.c"
#undef main

static int ctl[2], med[2], aud[2], fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } else printf("ok   %s\n", #c); } while (0)

static void
cmd(uint8_t sig)
{
	uint8_t b[3] = { 0x10, sig, 1 << 2 };	/* label 1, command */

	(void)write(ctl[1], b, sizeof(b));
	do_ctlreq(ctl[0], EV_READ, NULL);
}

static void
rsp(uint8_t sig)
{
	uint8_t b[2] = { 0x10 | RESPONSEACCEPT, sig };

	(void)write(ctl[1], b, sizeof(b));
	do_ctlreq(ctl[0], EV_READ, NULL);
}

/* The next message the "sink" received: (type << 8) | signal, or -1. */
static int
peer(void)
{
	uint8_t b[64];
	ssize_t n = read(ctl[1], b, sizeof(b));

	return n >= 2 ? ((b[0] & MESSAGETYPE) << 8) | b[1] : -1;
}

int
main(void)
{
	int16_t sound[512];
	int sc0, i;

	event_init();
	if (socketpair(AF_LOCAL, SOCK_DGRAM, 0, ctl) == -1 ||
	    socketpair(AF_LOCAL, SOCK_DGRAM, 0, med) == -1 || pipe(aud) == -1)
		err(1, "setup");
	fcntl(ctl[1], F_SETFL, O_NONBLOCK);
	fcntl(aud[0], F_SETFL, O_NONBLOCK);
	hc = ctl[0];
	sc = sc0 = med[0];
	audfile = aud[0];
	dontStop = true;
	state = ST_STREAMING;
	event_set(&interrupt_ev, audfile, EV_READ | EV_PERSIST, do_interrupt,
	    NULL);
	event_add(&interrupt_ev, NULL);

	printf("-- unrelated command while streaming\n");
	cmd(0x04);	/* GET_CONFIGURATION: rejected */
	CHECK(state == ST_STREAMING);
	CHECK(sc == sc0);
	(void)peer();

	printf("-- sink suspends\n");
	cmd(AVDTP_SUSPEND);
	CHECK(peer() == ((RESPONSEACCEPT << 8) | AVDTP_SUSPEND));
	CHECK(state == ST_SUSPENDED);
	CHECK(sc == sc0);
	CHECK(!event_pending(&interrupt_ev, EV_READ, NULL));
	CHECK(event_pending(&watch_ev, EV_READ, NULL));

	printf("-- silence while suspended\n");
	memset(sound, 0, sizeof(sound));
	(void)write(aud[1], sound, sizeof(sound));
	event_loop(EVLOOP_NONBLOCK);
	CHECK(state == ST_SUSPENDED);
	CHECK(peer() == -1);

	printf("-- sound again\n");
	for (i = 0; i < 512; i++)
		sound[i] = (int16_t)(i * 37);
	(void)write(aud[1], sound, sizeof(sound));
	event_loop(EVLOOP_NONBLOCK);
	CHECK(state == ST_STARTING);
	CHECK(peer() == ((COMMAND << 8) | AVDTP_START));
	CHECK(!event_pending(&watch_ev, EV_READ, NULL));

	printf("-- sink accepts START\n");
	rsp(AVDTP_START);
	CHECK(state == ST_STREAMING);
	CHECK(sc == sc0);
	CHECK(event_pending(&interrupt_ev, EV_READ, NULL));

	printf("-- sink suspends, then starts by itself\n");
	cmd(AVDTP_SUSPEND);
	(void)peer();
	CHECK(state == ST_SUSPENDED);
	cmd(AVDTP_START);
	CHECK(peer() == ((RESPONSEACCEPT << 8) | AVDTP_START));
	CHECK(state == ST_STREAMING);
	CHECK(event_pending(&interrupt_ev, EV_READ, NULL));
	CHECK(!event_pending(&watch_ev, EV_READ, NULL));
	CHECK(sc == sc0);

	printf("-- stream() failure with -n backs off instead of spinning\n");
	close(aud[1]);			/* EOF: always readable */
	do_interrupt(audfile, EV_READ, NULL);
	CHECK(!event_pending(&interrupt_ev, EV_READ, NULL));
	CHECK(event_pending(&retry_ev, EV_TIMEOUT, NULL));
	CHECK(retry_ms == 10);
	do_retry(-1, EV_TIMEOUT, NULL);
	do_interrupt(audfile, EV_READ, NULL);
	CHECK(retry_ms == 20);

	printf("%s (%d failed)\n", fails ? "FAILED" : "PASSED", fails);
	return fails != 0;
}
