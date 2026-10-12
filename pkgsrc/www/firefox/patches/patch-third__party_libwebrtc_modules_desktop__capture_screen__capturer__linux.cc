$NetBSD$

LispBSD: a wlr-screencopy screen capturer for wlroots compositors (no portal, no PipeWire).

--- third_party/libwebrtc/modules/desktop_capture/screen_capturer_linux.cc.orig
+++ third_party/libwebrtc/modules/desktop_capture/screen_capturer_linux.cc
@@ -23,6 +23,10 @@
 #include "modules/desktop_capture/linux/x11/screen_capturer_x11.h"
 #endif  // defined(WEBRTC_USE_X11)
 
+#if defined(WEBRTC_USE_WLR_SCREENCOPY)
+#include "modules/desktop_capture/linux/wayland_wlr/screen_capturer_wlr.h"
+#endif  // defined(WEBRTC_USE_WLR_SCREENCOPY)
+
 namespace webrtc {
 
 // static
@@ -37,6 +41,16 @@
   }
 #endif  // defined(WEBRTC_USE_PIPEWIRE)
 
+#if defined(WEBRTC_USE_WLR_SCREENCOPY)
+  // LispBSD: wlroots compositors without a portal or PipeWire.
+  if (DesktopCapturer::IsRunningUnderWayland() &&
+      ScreenCapturerWlr::IsSupported()) {
+    RTC_LOG(LS_INFO) << "DesktopCapturer::CreateRawScreenCapturer creates "
+                        "DesktopCapturer of type ScreenCapturerWlr";
+    return ScreenCapturerWlr::CreateRawScreenCapturer(options);
+  }
+#endif  // defined(WEBRTC_USE_WLR_SCREENCOPY)
+
 #if defined(WEBRTC_USE_X11)
   if (!DesktopCapturer::IsRunningUnderWayland())
     return ScreenCapturerX11::CreateRawScreenCapturer(options);
