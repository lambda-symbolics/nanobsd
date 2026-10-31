$NetBSD$

Metric calls skip the dispatcher when upload is off and no ping they
are sent in is enabled: the task would only drop the data.  Firefox with
telemetry off dispatched a few hundred of these a second during video
(glean.dispatcher ~1 % of a core, crossbeam yields).  The upload flag and
ping enablement are mirrored when their changes are dispatched.

--- third_party/rust/glean-core/src/lib.rs.orig
+++ third_party/rust/glean-core/src/lib.rs
@@ -20,7 +20,7 @@
 use std::collections::HashMap;
 use std::path::Path;
 use std::sync::atomic::{AtomicBool, Ordering};
-use std::sync::{Arc, Mutex};
+use std::sync::{Arc, Mutex, RwLock};
 use std::time::{Duration, UNIX_EPOCH};
 use std::{fmt, fs};
 
@@ -102,6 +102,15 @@
 /// Note: The initialization might still be in progress, as it runs in a separate thread.
 static INITIALIZE_CALLED: AtomicBool = AtomicBool::new(false);
 
+/// The upload (collection) flag as of the last change put on the dispatcher,
+/// and every ping constructed, by name.  With them a metric call can tell, on
+/// the caller's thread, that its task could store nothing and skip the
+/// dispatcher: upload off and every ping it is sent in off too.  True until
+/// Glean is initialized, so nothing is skipped before then.
+static UPLOAD_ENABLED_HINT: AtomicBool = AtomicBool::new(true);
+static PING_HINTS: Lazy<RwLock<HashMap<String, metrics::PingType>>> =
+    Lazy::new(|| RwLock::new(HashMap::new()));
+
 /// Keep track of the debug features before Glean is initialized.
 static PRE_INIT_DEBUG_VIEW_TAG: Mutex<String> = Mutex::new(String::new());
 static PRE_INIT_LOG_PINGS: AtomicBool = AtomicBool::new(false);
@@ -188,6 +197,40 @@
     dispatcher::launch(|| core::with_glean_mut(callback));
 }
 
+/// Launches a task that records into the metric `meta`, unless no ping it is
+/// sent in could store it by the time it runs (see `UPLOAD_ENABLED_HINT`).
+/// The task would drop the data itself; skipping it saves the dispatcher
+/// hand-off, a few hundred a second in Firefox with telemetry off.
+fn launch_recording(
+    meta: &common_metric_data::CommonMetricDataInternal,
+    callback: impl FnOnce(&Glean) + Send + 'static,
+) {
+    if metric_may_record(meta) {
+        launch_with_glean(callback);
+    }
+}
+
+fn metric_may_record(meta: &common_metric_data::CommonMetricDataInternal) -> bool {
+    if UPLOAD_ENABLED_HINT.load(Ordering::SeqCst) {
+        return true;
+    }
+    let pings = PING_HINTS.read().unwrap();
+    meta.storage_names().iter().any(|name| {
+        core::ALWAYS_ENABLED_PINGS.contains(&name.as_str())
+            || pings
+                .get(name)
+                .map_or(true, |ping| ping.enabled_without_upload_hint())
+    })
+}
+
+/// Remember a constructed ping for `metric_may_record`.
+pub(crate) fn ping_hint_add(ping: &PingType) {
+    PING_HINTS
+        .write()
+        .unwrap()
+        .insert(ping.name().to_string(), ping.clone());
+}
+
 /// Block on the dispatcher emptying.
 ///
 /// This will panic if called before Glean is initialized.
@@ -396,6 +439,7 @@
         log::error!("Glean should not be initialized multiple times");
         return;
     }
+    UPLOAD_ENABLED_HINT.store(cfg.upload_enabled, Ordering::SeqCst);
 
     let init_handle = thread::spawn("glean.init", move || {
         let upload_enabled = cfg.upload_enabled;
@@ -890,6 +934,7 @@
         return;
     }
 
+    UPLOAD_ENABLED_HINT.store(enabled, Ordering::SeqCst);
     crate::launch_with_glean_mut(move |glean| {
         let state = global_state().lock().unwrap();
         let original_enabled = glean.is_upload_enabled();
@@ -929,6 +974,7 @@
 /// Disabling a ping causes all data for that ping to be removed from storage
 /// and all pending pings of that type to be deleted.
 pub fn set_ping_enabled(ping: &PingType, enabled: bool) {
+    ping.store_enabled_hint(enabled);
     let ping = ping.clone();
     if was_initialize_called() && core::global_glean().is_some() {
         crate::launch_with_glean_mut(move |glean| glean.set_ping_enabled(&ping, enabled));
@@ -1286,6 +1332,7 @@
 
         // Allow us to go through initialization again.
         INITIALIZE_CALLED.store(false, Ordering::SeqCst);
+        UPLOAD_ENABLED_HINT.store(true, Ordering::SeqCst);
     } else if clear_stores {
         if let Some(data_path) = data_path {
             let _ = std::fs::remove_dir_all(data_path).ok();
