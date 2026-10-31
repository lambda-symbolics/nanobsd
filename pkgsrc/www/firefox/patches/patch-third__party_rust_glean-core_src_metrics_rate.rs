$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/rate.rs.orig
+++ third_party/rust/glean-core/src/metrics/rate.rs
@@ -68,7 +68,7 @@
     /// Logs an error if the `amount` is negative.
     pub fn add_to_numerator(&self, amount: i32) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.add_to_numerator_sync(glean, amount))
+        crate::launch_recording(self.meta(), move |glean| metric.add_to_numerator_sync(glean, amount))
     }
 
     #[doc(hidden)]
@@ -108,7 +108,7 @@
     /// Logs an error if the `amount` is negative.
     pub fn add_to_denominator(&self, amount: i32) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.add_to_denominator_sync(glean, amount))
+        crate::launch_recording(self.meta(), move |glean| metric.add_to_denominator_sync(glean, amount))
     }
 
     #[doc(hidden)]
