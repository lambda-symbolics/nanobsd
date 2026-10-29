$NetBSD$

Run the data callback once per device block (audio(4) blocksize, 10 ms by
default) instead of every 32 frames: 1500 callbacks and writes a second at
48 kHz woke the audio graph for nothing.

--- media/libcubeb/src/cubeb_sun.c.orig
+++ media/libcubeb/src/cubeb_sun.c
@@ -41,6 +41,11 @@
 #define SUN_BUFFER_FRAMES (32)
 #endif
 
+/* Upper bound for the per-callback frame count taken from the device. */
+#ifndef SUN_MAX_BUFFER_FRAMES
+#define SUN_MAX_BUFFER_FRAMES (8192)
+#endif
+
 /*
  * Supported on NetBSD regardless of hardware.
  */
@@ -89,6 +94,7 @@
   cubeb_state_callback state_cb;
   uint64_t frames_written;
   uint64_t blocks_written;
+  unsigned nframes; /* frames per data callback: one device block */
 };
 
 int
@@ -442,10 +448,10 @@
     pthread_mutex_unlock(&s->mutex);
     if (s->record.fd != -1 && s->record.floating) {
       sun_linear32_to_float(s->record.buf,
-                            s->record.info.record.channels * SUN_BUFFER_FRAMES);
+                            s->record.info.record.channels * s->nframes);
     }
     to_write = s->data_cb(s, s->user_ptr, s->record.buf, s->play.buf,
-                          SUN_BUFFER_FRAMES);
+                          s->nframes);
     if (to_write == CUBEB_ERROR) {
       state = CUBEB_STATE_ERROR;
       break;
@@ -465,11 +471,11 @@
                              vol);
       }
     }
-    if (to_write < SUN_BUFFER_FRAMES) {
+    if (to_write < s->nframes) {
       drain = 1;
     }
     to_write = s->play.fd != -1 ? to_write : 0;
-    to_read = s->record.fd != -1 ? SUN_BUFFER_FRAMES : 0;
+    to_read = s->record.fd != -1 ? s->nframes : 0;
     write_ofs = 0;
     read_ofs = 0;
     while (to_write > 0 || to_read > 0) {
@@ -606,15 +612,34 @@
   }
   s->play.frame_size =
       s->play.info.play.channels * (s->play.info.play.precision / 8);
+  s->record.frame_size =
+      s->record.info.record.channels * (s->record.info.record.precision / 8);
+  /*
+   * Run the data callback once per device block (audio(4) reports the
+   * track's block, 10 ms by default) rather than every SUN_BUFFER_FRAMES:
+   * 32 frames meant 1500 callbacks and writes a second at 48 kHz, each
+   * waking the caller's audio graph, for no gain in latency since the
+   * device only consumes whole blocks.
+   */
+  s->nframes = SUN_BUFFER_FRAMES;
+  if (s->play.fd != -1 && s->play.frame_size > 0 &&
+      s->play.info.blocksize / s->play.frame_size > s->nframes) {
+    s->nframes = s->play.info.blocksize / s->play.frame_size;
+  } else if (s->play.fd == -1 && s->record.fd != -1 &&
+             s->record.frame_size > 0 &&
+             s->record.info.blocksize / s->record.frame_size > s->nframes) {
+    s->nframes = s->record.info.blocksize / s->record.frame_size;
+  }
+  if (s->nframes > SUN_MAX_BUFFER_FRAMES) {
+    s->nframes = SUN_MAX_BUFFER_FRAMES;
+  }
   if (s->play.fd != -1 &&
-      (s->play.buf = calloc(SUN_BUFFER_FRAMES, s->play.frame_size)) == NULL) {
+      (s->play.buf = calloc(s->nframes, s->play.frame_size)) == NULL) {
     ret = CUBEB_ERROR;
     goto error;
   }
-  s->record.frame_size =
-      s->record.info.record.channels * (s->record.info.record.precision / 8);
   if (s->record.fd != -1 &&
-      (s->record.buf = calloc(SUN_BUFFER_FRAMES, s->record.frame_size)) ==
+      (s->record.buf = calloc(s->nframes, s->record.frame_size)) ==
           NULL) {
     ret = CUBEB_ERROR;
     goto error;
