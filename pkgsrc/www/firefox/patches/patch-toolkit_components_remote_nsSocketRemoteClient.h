$NetBSD$

LISPBSD: hand URLs to the running instance over a Unix socket in
XDG_RUNTIME_DIR (the X11 remote needs an X server, the D-Bus one a
session bus; a plain Wayland session has neither).

--- toolkit/components/remote/nsSocketRemoteClient.h.orig
+++ toolkit/components/remote/nsSocketRemoteClient.h
@@ -0,0 +1,35 @@
+/* This Source Code Form is subject to the terms of the Mozilla Public
+ * License, v. 2.0. If a copy of the MPL was not distributed with this
+ * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
+
+/*
+ * LISPBSD: remoting over a Unix socket in $XDG_RUNTIME_DIR.  The X11
+ * remote needs an X server and the D-Bus one a session bus; a Wayland
+ * session without either could not hand a URL to the running instance.
+ */
+
+#ifndef TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTECLIENT_H_
+#define TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTECLIENT_H_
+
+#include "nsRemoteClient.h"
+#include "nsString.h"
+
+// The socket of the instance running PROGRAM with PROFILE.
+bool nsSocketRemotePath(const char* aProgram, const char* aProfile,
+                        nsACString& aPath);
+
+class nsSocketRemoteClient : public nsRemoteClient {
+ public:
+  explicit nsSocketRemoteClient(nsACString& aStartupToken)
+      : mStartupToken(aStartupToken) {}
+
+  nsresult Init() override { return NS_OK; }
+  nsresult SendCommandLine(const char* aProgram, const char* aProfile,
+                           int32_t argc, const char** argv,
+                           bool aRaise) override;
+
+ private:
+  nsACString& mStartupToken;
+};
+
+#endif  // TOOLKIT_COMPONENTS_REMOTE_NSSOCKETREMOTECLIENT_H_
