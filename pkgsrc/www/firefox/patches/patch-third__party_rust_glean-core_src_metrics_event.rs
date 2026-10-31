$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/event.rs.orig
+++ third_party/rust/glean-core/src/metrics/event.rs
@@ -75,7 +75,7 @@
         let now = Utc::now();
         let precise_timestamp = now.timestamp_millis() as u64;
 
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             let sent = metric.record_sync(glean, timestamp, extra, precise_timestamp);
             if sent {
                 let state = crate::global_state().lock().unwrap();
