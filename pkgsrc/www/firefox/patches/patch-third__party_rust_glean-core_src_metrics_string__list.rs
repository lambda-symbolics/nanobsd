$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/string_list.rs.orig
+++ third_party/rust/glean-core/src/metrics/string_list.rs
@@ -55,7 +55,7 @@
     /// Truncates the value if it is longer than `MAX_STRING_LENGTH` bytes and logs an error.
     pub fn add(&self, value: String) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.add_sync(glean, value))
+        crate::launch_recording(self.meta(), move |glean| metric.add_sync(glean, value))
     }
 
     /// Adds a new string to the list synchronously
@@ -107,7 +107,7 @@
     /// Truncates any value in the list if it is longer than `MAX_STRING_LENGTH` and logs an error.
     pub fn set(&self, values: Vec<String>) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, values))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, values))
     }
 
     /// Sets to a specific list of strings synchronously.
