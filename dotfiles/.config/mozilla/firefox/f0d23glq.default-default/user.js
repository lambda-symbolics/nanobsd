user_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);

// --- power-saving knobs (added 2026-09-19) ---
// Rendering / GPU repaint rate (targets the i915 msi5 interrupt storm)
user_pref("layout.frame_rate", 30);
user_pref("image.animation_mode", "once");
user_pref("toolkit.cosmeticAnimations.enabled", false);
// Background-tab timer throttling (fewer CPU wakeups)
user_pref("dom.min_background_timeout_value", 10000);
// Cap content processes (4-core box; less overhead/memory/wakeup surface)
user_pref("dom.ipc.processCount", 4);
// Session store to disk every 60s instead of 15s (fewer NVMe wakeups)
user_pref("browser.sessionstore.interval", 60000);
// Block autoplay video+audio (no background CPU/GPU spin)
user_pref("media.autoplay.default", 5);
// Kill periodic telemetry / background network wakeups
user_pref("toolkit.telemetry.enabled", false);
user_pref("toolkit.telemetry.unified", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("app.shield.optoutstudies.enabled", false);
user_pref("browser.newtabpage.activity-stream.feeds.section.topstories", false);
user_pref("browser.newtabpage.activity-stream.showSponsored", false);
user_pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
user_pref("network.prefetch-next", false);
user_pref("network.predictor.enabled", false);
