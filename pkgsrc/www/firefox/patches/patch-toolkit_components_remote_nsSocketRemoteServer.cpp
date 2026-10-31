$NetBSD$

LISPBSD: hand URLs to the running instance over a Unix socket in
XDG_RUNTIME_DIR (the X11 remote needs an X server, the D-Bus one a
session bus; a plain Wayland session has neither).

--- toolkit/components/remote/nsSocketRemoteServer.cpp.orig
+++ toolkit/components/remote/nsSocketRemoteServer.cpp
@@ -0,0 +1,113 @@
+/* This Source Code Form is subject to the terms of the Mozilla Public
+ * License, v. 2.0. If a copy of the MPL was not distributed with this
+ * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
+
+#include "nsSocketRemoteServer.h"
+#include "nsSocketRemoteClient.h"
+#include "mozilla/Span.h"
+#include "mozilla/UniquePtr.h"
+
+#include <glib.h>
+#include <glib-unix.h>
+#include <errno.h>
+#include <fcntl.h>
+#include <poll.h>
+#include <string.h>
+#include <sys/socket.h>
+#include <sys/stat.h>
+#include <sys/un.h>
+#include <unistd.h>
+
+// A client gets this long to deliver its command line; it is a local
+// process that has written it in one go, so this only guards the main
+// thread against a stuck peer.
+static const int kReadTimeoutMs = 2000;
+static const uint32_t kMaxCommandLine = 1 << 20;
+
+static bool ReadAll(int aFd, char* aBuf, size_t aLen) {
+  struct pollfd pfd = {aFd, POLLIN, 0};
+  while (aLen > 0) {
+    if (poll(&pfd, 1, kReadTimeoutMs) <= 0) return false;
+    ssize_t n = read(aFd, aBuf, aLen);
+    if (n < 0 && errno == EINTR) continue;
+    if (n <= 0) return false;
+    aBuf += n;
+    aLen -= n;
+  }
+  return true;
+}
+
+static gboolean OnIncoming(gint aFd, GIOCondition aCondition,
+                           gpointer aUserData) {
+  static_cast<nsSocketRemoteServer*>(aUserData)->HandleConnection();
+  return G_SOURCE_CONTINUE;
+}
+
+void nsSocketRemoteServer::HandleConnection() {
+  int conn = accept(mFd, nullptr, nullptr);
+  if (conn < 0) {
+    return;
+  }
+  // The listening socket is non-blocking; this one must not be.
+  fcntl(conn, F_SETFL, fcntl(conn, F_GETFL) & ~O_NONBLOCK);
+
+  const char* reply = "500 command not parseable";
+  uint32_t len = 0;
+  if (ReadAll(conn, reinterpret_cast<char*>(&len), sizeof(len)) && len > 0 &&
+      len <= kMaxCommandLine) {
+    mozilla::UniquePtr<char[]> buf(new char[len]);
+    if (ReadAll(conn, buf.get(), len)) {
+      reply = HandleCommandLine(mozilla::Span(buf.get(), len), 0);
+    }
+  }
+  (void)!write(conn, reply, strlen(reply));
+  close(conn);
+}
+
+nsresult nsSocketRemoteServer::Startup(const char* aAppName,
+                                       const char* aProfileName) {
+  if (mFd >= 0) {
+    return NS_OK;
+  }
+  if (!nsSocketRemotePath(aAppName, aProfileName, mPath)) {
+    return NS_ERROR_INVALID_ARG;
+  }
+
+  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
+  if (fd < 0) {
+    return NS_ERROR_FAILURE;
+  }
+  fcntl(fd, F_SETFD, FD_CLOEXEC);
+  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
+
+  struct sockaddr_un sun;
+  memset(&sun, 0, sizeof(sun));
+  sun.sun_family = AF_UNIX;
+  strlcpy(sun.sun_path, mPath.get(), sizeof(sun.sun_path));
+  // We hold the profile lock, so a socket left here is from an instance
+  // that died.
+  unlink(mPath.get());
+  mode_t old = umask(077);
+  int rv = bind(fd, (struct sockaddr*)&sun, sizeof(sun));
+  umask(old);
+  if (rv != 0 || listen(fd, 8) != 0) {
+    close(fd);
+    return NS_ERROR_FAILURE;
+  }
+
+  mFd = fd;
+  mSource = g_unix_fd_add(mFd, G_IO_IN, OnIncoming, this);
+  return NS_OK;
+}
+
+void nsSocketRemoteServer::Shutdown() {
+  if (mSource) {
+    g_source_remove(mSource);
+    mSource = 0;
+  }
+  if (mFd >= 0) {
+    close(mFd);
+    mFd = -1;
+    unlink(mPath.get());
+  }
+}
