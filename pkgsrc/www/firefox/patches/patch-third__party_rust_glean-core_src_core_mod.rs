$NetBSD$

The always-enabled storage names, shared with the dispatcher skip in
lib.rs; ping enablement changes keep the hint in step.

--- third_party/rust/glean-core/src/core/mod.rs.orig
+++ third_party/rust/glean-core/src/core/mod.rs
@@ -37,6 +37,16 @@
 };
 
 const CLIENT_ID_PLAIN_FILENAME: &str = "client_id.txt";
+
+/// Storage names that are always enabled: we "abuse" pings/storage names for
+/// internal data.
+pub(crate) const ALWAYS_ENABLED_PINGS: &[&str] = &[
+    "glean_client_info",
+    "glean_internal_info",
+    // for `experimentation_id`.
+    // That should probably have gone into `glean_internal_info` instead.
+    "all-pings",
+];
 static GLEAN: OnceCell<Mutex<Glean>> = OnceCell::new();
 
 /// Rate limiting defaults
@@ -762,6 +772,7 @@
     #[doc(hidden)]
     pub fn set_ping_enabled(&mut self, ping: &PingType, enabled: bool) {
         ping.store_enabled(enabled);
+        ping.store_enabled_hint(enabled);
         if !enabled {
             if let Some(data) = self.data_store.as_ref() {
                 _ = data.clear_ping_lifetime_storage(ping.name());
@@ -792,17 +803,8 @@
     /// Only data for enabled pings is recorded.
     /// Disabled pings are never submitted.
     pub fn is_ping_enabled(&self, ping: &str) -> bool {
-        // We "abuse" pings/storage names for internal data.
-        const DEFAULT_ENABLED: &[&str] = &[
-            "glean_client_info",
-            "glean_internal_info",
-            // for `experimentation_id`.
-            // That should probably have gone into `glean_internal_info` instead.
-            "all-pings",
-        ];
-
         // `client_info`-like stuff is always enabled.
-        if DEFAULT_ENABLED.contains(&ping) {
+        if ALWAYS_ENABLED_PINGS.contains(&ping) {
             return true;
         }
 
