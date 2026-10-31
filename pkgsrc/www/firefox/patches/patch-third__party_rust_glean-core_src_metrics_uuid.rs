$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/uuid.rs.orig
+++ third_party/rust/glean-core/src/metrics/uuid.rs
@@ -47,7 +47,7 @@
     /// * `value` - The [`Uuid`] to set the metric to.
     pub fn set(&self, value: String) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, &value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, &value))
     }
 
     /// Sets to the specified value synchronously.
@@ -90,7 +90,7 @@
 
         let value = uuid.to_string();
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, value));
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, value));
 
         uuid.to_string()
     }
