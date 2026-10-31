$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/boolean.rs.orig
+++ third_party/rust/glean-core/src/metrics/boolean.rs
@@ -77,7 +77,7 @@
     /// * `value` - the value to set.
     pub fn set(&self, value: bool) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, value))
     }
 
     /// **Test-only API (exported for FFI purposes).**
