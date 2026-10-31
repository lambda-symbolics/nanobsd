$NetBSD$

Pings remember their enablement as of the last change put on the
dispatcher, and register for the dispatcher skip in lib.rs.

--- third_party/rust/glean-core/src/metrics/ping.rs.orig
+++ third_party/rust/glean-core/src/metrics/ping.rs
@@ -34,6 +34,8 @@
     pub include_info_sections: bool,
     /// Whether this ping is enabled.
     pub enabled: AtomicBool,
+    /// `enabled` as of the last change put on the dispatcher.
+    enabled_hint: AtomicBool,
     /// Other pings that should be scheduled when this ping is sent.
     pub schedules_pings: Vec<String>,
     /// The "reason" codes that this ping can send
@@ -141,6 +143,7 @@
             precise_timestamps,
             include_info_sections,
             enabled: AtomicBool::new(enabled),
+            enabled_hint: AtomicBool::new(enabled),
             schedules_pings,
             reason_codes,
             follows_collection_enabled: AtomicBool::new(follows_collection_enabled),
@@ -149,6 +152,7 @@
 
         // Register this ping.
         // That will happen asynchronously and not block operation.
+        crate::ping_hint_add(&this);
         crate::register_ping_type(&this);
 
         this
@@ -195,6 +199,19 @@
         self.0.enabled.store(enabled, Ordering::Release);
     }
 
+    /// Note an enable/disable when it is dispatched, before it runs.
+    pub(crate) fn store_enabled_hint(&self, enabled: bool) {
+        self.0.enabled_hint.store(enabled, Ordering::SeqCst);
+    }
+
+    /// Whether data for this ping may be stored while upload is disabled,
+    /// counting enable/disable calls still on the dispatcher.  Remote
+    /// settings only apply to pings that follow upload, which are off then.
+    pub(crate) fn enabled_without_upload_hint(&self) -> bool {
+        !self.0.follows_collection_enabled.load(Ordering::Relaxed)
+            && self.0.enabled_hint.load(Ordering::SeqCst)
+    }
+
     pub(crate) fn enabled(&self, glean: &Glean) -> bool {
         if self.0.follows_collection_enabled.load(Ordering::Relaxed) {
             // if this follows collection_enabled:
