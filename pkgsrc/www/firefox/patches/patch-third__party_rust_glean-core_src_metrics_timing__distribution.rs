$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/timing_distribution.rs.orig
+++ third_party/rust/glean-core/src/metrics/timing_distribution.rs
@@ -156,7 +156,7 @@
         let start_time = zeitstempel::now_awake();
         let id = self.next_id.fetch_add(1, Ordering::SeqCst).into();
         let metric = self.clone();
-        crate::launch_with_glean(move |_glean| metric.set_start(id, start_time));
+        crate::launch_recording(self.meta(), move |_glean| metric.set_start(id, start_time));
         id
     }
 
@@ -194,7 +194,7 @@
     pub fn stop_and_accumulate(&self, id: TimerId) {
         let stop_time = zeitstempel::now_awake();
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_stop_and_accumulate(glean, id, stop_time));
+        crate::launch_recording(self.meta(), move |glean| metric.set_stop_and_accumulate(glean, id, stop_time));
     }
 
     fn set_stop(&self, id: TimerId, stop_time: u64) -> Result<u64, (ErrorType, &str)> {
@@ -298,7 +298,7 @@
     ///   same timing distribution metric.
     pub fn cancel(&self, id: TimerId) {
         let metric = self.clone();
-        crate::launch_with_glean(move |_glean| metric.cancel_sync(id));
+        crate::launch_recording(self.meta(), move |_glean| metric.cancel_sync(id));
     }
 
     /// Aborts a previous [`start`](Self::start) call synchronously.
@@ -330,7 +330,7 @@
     /// are longer than `MAX_SAMPLE_TIME`.
     pub fn accumulate_samples(&self, samples: Vec<i64>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_samples_sync(glean, &samples))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_samples_sync(glean, &samples))
     }
 
     /// Accumulates precisely one signed sample and appends it to the metric.
@@ -358,7 +358,7 @@
     /// `MAX_SAMPLE_TIME`.
     pub fn accumulate_single_sample(&self, sample: i64) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.accumulate_samples_sync(glean, &[sample]))
+        crate::launch_recording(self.meta(), move |glean| metric.accumulate_samples_sync(glean, &[sample]))
     }
 
     /// **Test-only API (exported for testing purposes).**
@@ -444,7 +444,7 @@
     /// are longer than `MAX_SAMPLE_TIME`.
     pub fn accumulate_raw_samples_nanos(&self, samples: Vec<u64>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             metric.accumulate_raw_samples_nanos_sync(glean, &samples)
         })
     }
@@ -473,7 +473,7 @@
     pub fn accumulate_raw_duration(&self, duration: Duration) {
         let duration_ns = duration.as_nanos().try_into().unwrap_or(u64::MAX);
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             metric.accumulate_raw_samples_nanos_sync(glean, &[duration_ns])
         })
     }
@@ -582,7 +582,7 @@
 
     fn commit_histogram(&self, histogram: Histogram<Functional>, errors: usize) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             if errors > 0 {
                 let max_sample_time = metric.time_unit.as_nanos(MAX_SAMPLE_TIME);
                 let msg = format!(
