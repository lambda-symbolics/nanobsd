$NetBSD$

LISPBSD: libsndio on NetBSD audio(4) through ossaudio(3).
- sio_flush(): SNDCTL_DSP_SETTRIGGER only pauses the device and keeps the
  queued samples; discard them with SNDCTL_DSP_HALT_OUTPUT (AUDIO_FLUSH).
  Otherwise the next sio_write() blocked on the paused device for ever
  (mpv hung on every seek, pause and track switch).
- start: audio(4) converts up to four blocks into the track output ring as
  soon as they are written and reports them as gone from the queue, so a
  client sizing its writes from the reported space in whole blocks (mpv)
  can stop one block short of bufsz; start when within a block of full,
  not only when exactly full (mpv never resumed after a pause).

--- libsndio/sio_oss.c.orig	2026-10-08 13:11:54.549679461 +0200
+++ libsndio/sio_oss.c	2026-10-08 13:44:10.351491902 +0200
@@ -387,10 +387,27 @@
 	struct sio_oss_hdl *hdl = (struct sio_oss_hdl*)sh;
 	int trig;
 
+#if defined(__NetBSD__)
+	/*
+	 * ossaudio(3) maps SNDCTL_DSP_SETTRIGGER to pausing the device and
+	 * keeps the queued samples, unlike OSS where clearing the output bit
+	 * discards them.  Discard them here (SNDCTL_DSP_HALT_OUTPUT is
+	 * AUDIO_FLUSH), otherwise sio_oss_start() refills a buffer that is
+	 * still partly full, the write blocks on the paused device, and
+	 * sio_oss_pollfd() never sees the fill level that starts it.
+	 */
+	if (ioctl(hdl->fd, SNDCTL_DSP_HALT_OUTPUT, NULL) == -1) {
+		DPERROR("sio_oss_flush: HALT_OUTPUT");
+		hdl->sio.eof = 1;
+		return 0;
+	}
+	hdl->filling = 0;
+#else
 	if (hdl->filling) {
 		hdl->filling = 0;
 		return 1;
 	}
+#endif
 	trig = 0;
 	if (ioctl(hdl->fd, SNDCTL_DSP_SETTRIGGER, &trig) == -1) {
 		DPERROR("sio_oss_flush: SETTRIGGER");
@@ -639,8 +656,22 @@
 
 	pfd->fd = hdl->fd;
 	pfd->events = events;
+#if defined(__NetBSD__)
+	/*
+	 * Start once the buffer is within one block of full, not only when
+	 * it is exactly full: audio(4) moves up to four blocks into the
+	 * track's output ring as soon as they are written and reports them
+	 * as gone from the queue, so a client that sizes its writes from
+	 * the reported space in whole blocks (mpv) can stop short of bufsz
+	 * and the device would never be started.
+	 */
+	if (hdl->filling && hdl->sio.wused + hdl->sio.par.round *
+	    hdl->sio.par.pchan * hdl->sio.par.bps > hdl->sio.par.bufsz *
+	    hdl->sio.par.pchan * hdl->sio.par.bps) {
+#else
 	if (hdl->filling && hdl->sio.wused == hdl->sio.par.bufsz *
 		hdl->sio.par.pchan * hdl->sio.par.bps) {
+#endif
 		hdl->filling = 0;
 		trig = 0;
 		if (hdl->sio.mode & SIO_PLAY)
