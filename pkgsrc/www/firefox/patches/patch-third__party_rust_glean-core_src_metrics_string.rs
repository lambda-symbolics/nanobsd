$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/string.rs.orig
+++ third_party/rust/glean-core/src/metrics/string.rs
@@ -69,7 +69,7 @@
     /// Truncates the value if it is longer than `MAX_LENGTH_VALUE` bytes and logs an error.
     pub fn set(&self, value: String) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, &value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, &value))
     }
 
     /// Sets to the specified value synchronously.
