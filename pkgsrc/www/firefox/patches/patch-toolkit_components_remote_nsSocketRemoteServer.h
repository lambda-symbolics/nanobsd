$NetBSD$

LISPBSD: hand URLs to the running instance over a Unix socket in
XDG_RUNTIME_DIR (the X11 remote needs an X server, the D-Bus one a
session bus; a plain Wayland session has neither).

--- toolkit/components/remote/nsSocketRemoteServer.h.orig
+++ toolkit/components/remote/nsSocketRemoteServer.h
@@ -0,0 +1,29 @@
+/* This Source Code Form is subject to the terms of the Mozilla Public
+ * License, v. 2.0. If a copy of the MPL was not distributed with this
+ * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
+
+#ifndef TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTESERVER_H_
+#define TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTESERVER_H_
+
+#include "nsRemoteServer.h"
+#include "nsUnixRemoteServer.h"
+#include "nsString.h"
+
+// LISPBSD: the server side of nsSocketRemoteClient, on the GLib main loop.
+class nsSocketRemoteServer final : public nsRemoteServer,
+                                   public nsUnixRemoteServer {
+ public:
+  ~nsSocketRemoteServer() override { Shutdown(); }
+
+  nsresult Startup(const char* aAppName, const char* aProfileName) override;
+  void Shutdown() override;
+
+  void HandleConnection();
+
+ private:
+  int mFd = -1;
+  unsigned mSource = 0;
+  nsCString mPath;
+};
+
+#endif  // TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTESERVER_H_
