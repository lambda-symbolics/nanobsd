$NetBSD$

Record through launch_recording: skipped when no enabled ping would
store the value (see lib.rs).

--- third_party/rust/glean-core/src/metrics/object.rs.orig
+++ third_party/rust/glean-core/src/metrics/object.rs
@@ -66,7 +66,7 @@
     /// * `value` - the value to set.
     pub fn set(&self, value: JsonValue) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| metric.set_sync(glean, value))
+        crate::launch_recording(self.meta(), move |glean| metric.set_sync(glean, value))
     }
 
     /// Sets to the specified structure.
@@ -81,7 +81,7 @@
     /// * `object` - JSON representation of the object to set.
     pub fn set_string(&self, object: String) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             let object = match serde_json::from_str(&object) {
                 Ok(object) => object,
                 Err(_) => {
@@ -101,7 +101,7 @@
     // errors
     pub fn record_schema_error(&self) {
         let metric = self.clone();
-        crate::launch_with_glean(move |glean| {
+        crate::launch_recording(self.meta(), move |glean| {
             let msg = "Value did not match predefined schema";
             record_error(glean, &metric.meta, ErrorType::InvalidValue, msg, None);
         });
