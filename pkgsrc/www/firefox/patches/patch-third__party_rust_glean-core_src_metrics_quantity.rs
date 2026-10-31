$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/quantity.rs.orig
+++ third_party/rust/glean-core/src/metrics/quantity.rs
@@ -65,7 +65,7 @@
     /// Logs an error if the `value` is negative.
     pub fn set(&self, value: i64) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, value))
     }
 
     /// Sets the value synchronously. Must be non-negative.
