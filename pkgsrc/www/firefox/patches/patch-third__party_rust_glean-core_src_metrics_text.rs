$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/text.rs.orig
+++ third_party/rust/glean-core/src/metrics/text.rs
@@ -72,7 +72,7 @@
     /// and logs an error.
     pub fn set(&self, value: String) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, &value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, &value))
     }
 
     /// Sets to the specified value synchronously,
