$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/timespan.rs.orig
+++ third_party/rust/glean-core/src/metrics/timespan.rs
@@ -59,7 +59,7 @@
         let start_time = zeitstempel::now_awake();
 
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_start(glean, start_time));
+        crate::launch_recording(self.meta(), move |glean| metric.set_start(glean, start_time));
     }
 
     /// Set start time synchronously.
@@ -91,7 +91,7 @@
         let stop_time = zeitstempel::now_awake();
 
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_stop(glean, stop_time));
+        crate::launch_recording(self.meta(), move |glean| metric.set_stop(glean, stop_time));
     }
 
     /// Set stop time synchronously.
@@ -169,7 +169,7 @@
     /// * `elapsed` - The elapsed time to record.
     pub fn set_raw(&self, elapsed: Duration) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_raw_sync(glean, elapsed));
+        crate::launch_recording(self.meta(), move |glean| metric.set_raw_sync(glean, elapsed));
     }
 
     /// Explicitly sets the timespan value in nanoseconds.
