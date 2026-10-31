$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/custom_distribution.rs.orig
+++ third_party/rust/glean-core/src/metrics/custom_distribution.rs
@@ -107,7 +107,7 @@
     /// for each of them.
     pub fn accumulate_samples(&self, samples: Vec<i64>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_samples_sync(glean, &samples))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_samples_sync(glean, &samples))
     }
 
     /// Accumulates precisely one signed sample and appends it to the metric.
@@ -126,7 +126,7 @@
     /// [`ErrorType::InvalidValue`].
     pub fn accumulate_single_sample(&self, sample: i64) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_samples_sync(glean, &[sample]))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_samples_sync(glean, &[sample]))
     }
 
     /// Accumulates the provided sample in the metric synchronously.
@@ -260,7 +260,7 @@
 
     fn commit_histogram(&self, histogram: Histogram<LinearOrExponential>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             glean
                 .storage()
                 .record_with(glean, &metric.meta, move |old_value| {
