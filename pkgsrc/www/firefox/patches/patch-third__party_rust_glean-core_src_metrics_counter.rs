$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/counter.rs.orig
+++ third_party/rust/glean-core/src/metrics/counter.rs
@@ -112,7 +112,7 @@
     /// Logs an error if the `amount` is 0 or negative.
     pub fn add(&self, amount: i32) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.add_sync(glean, amount))
+        crate::launch_recording(self.meta(), move |glean| metric.add_sync(glean, amount))
     }
 
     /// Get current value
