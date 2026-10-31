$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/datetime.rs.orig
+++ third_party/rust/glean-core/src/metrics/datetime.rs
@@ -133,7 +133,7 @@
     /// * `dt` - the optinal datetime to set this to. If missing the current date is used.
     pub fn set(&self, dt: Option<Datetime>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             metric.set_sync(glean, dt);
         })
     }
