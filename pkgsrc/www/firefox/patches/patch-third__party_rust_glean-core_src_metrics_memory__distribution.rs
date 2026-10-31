$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/memory_distribution.rs.orig
+++ third_party/rust/glean-core/src/metrics/memory_distribution.rs
@@ -100,7 +100,7 @@
     /// and an [`ErrorType::InvalidValue`] error is recorded.
     pub fn accumulate(&self, sample: i64) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_sync(glean, sample))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_sync(glean, sample))
     }
 
     /// Accumulates the provided sample in the metric synchronously.
@@ -182,7 +182,7 @@
     /// and an [`ErrorType::InvalidValue`] error is recorded.
     pub fn accumulate_samples(&self, samples: Vec<i64>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_samples_sync(glean, samples))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_samples_sync(glean, samples))
     }
 
     /// Accumulates the provided signed samples in the metric synchronously.
@@ -297,7 +297,7 @@
 
     fn commit_histogram(&self, histogram: Histogram<Functional>, errors: usize) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             if errors > 0 {
                 let msg = format!("Accumulated {} samples larger than 1TB", errors);
                 record_error(
