$NetBSD$

LISPBSD: hand URLs to the running instance over a Unix socket in
XDG_RUNTIME_DIR (the X11 remote needs an X server, the D-Bus one a
session bus; a plain Wayland session has neither).

--- toolkit/components/remote/nsSocketRemoteClient.cpp.orig
+++ toolkit/components/remote/nsSocketRemoteClient.cpp
@@ -0,0 +1,111 @@
+/* This Source Code Form is subject to the terms of the Mozilla Public
+ * License, v. 2.0. If a copy of the MPL was not distributed with this
+ * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
+
+#include "nsSocketRemoteClient.h"
+#include "RemoteUtils.h"
+#include "mozilla/Logging.h"
+
+#include <glib.h>
+#include <errno.h>
+#include <poll.h>
+#include <string.h>
+#include <sys/socket.h>
+#include <sys/un.h>
+#include <unistd.h>
+
+static mozilla::LazyLogModule sRemoteLm("nsSocketRemoteClient");
+#define LOG(...) MOZ_LOG(sRemoteLm, mozilla::LogLevel::Debug, (__VA_ARGS__))
+
+// How long the running instance may take to act on the command line.
+static const int kReplyTimeoutMs = 20000;
+
+static void AppendSanitized(nsACString& aOut, const char* aName) {
+  for (const char* p = aName; *p && p - aName < 40; p++) {
+    char c = *p;
+    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
+              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
+    aOut.Append(ok ? c : '_');
+  }
+}
+
+bool nsSocketRemotePath(const char* aProgram, const char* aProfile,
+                        nsACString& aPath) {
+  if (!aProgram || !aProfile || !*aProfile) {
+    return false;
+  }
+  aPath.Assign(g_get_user_runtime_dir());
+  aPath.AppendLiteral("/mozilla-remote-");
+  AppendSanitized(aPath, aProgram);
+  aPath.Append('-');
+  AppendSanitized(aPath, aProfile);
+  return aPath.Length() < sizeof(((struct sockaddr_un*)nullptr)->sun_path);
+}
+
+static bool WriteAll(int aFd, const char* aBuf, size_t aLen) {
+  while (aLen > 0) {
+    ssize_t n = write(aFd, aBuf, aLen);
+    if (n < 0 && errno == EINTR) continue;
+    if (n <= 0) return false;
+    aBuf += n;
+    aLen -= n;
+  }
+  return true;
+}
+
+nsresult nsSocketRemoteClient::SendCommandLine(const char* aProgram,
+                                               const char* aProfile,
+                                               int32_t argc, const char** argv,
+                                               bool aRaise) {
+  nsAutoCString path;
+  if (!nsSocketRemotePath(aProgram, aProfile, path)) {
+    return NS_ERROR_INVALID_ARG;
+  }
+  LOG("nsSocketRemoteClient: %s", path.get());
+
+  int len = 0;
+  mozilla::UniquePtr<char[]> commandLine = ConstructCommandLine(
+      argc, argv,
+      mStartupToken.IsEmpty() ? nullptr
+                              : PromiseFlatCString(mStartupToken).get(),
+      &len);
+  if (!commandLine || len <= 0) {
+    return NS_ERROR_FAILURE;
+  }
+
+  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
+  if (fd < 0) {
+    return NS_ERROR_FAILURE;
+  }
+  struct sockaddr_un sun;
+  memset(&sun, 0, sizeof(sun));
+  sun.sun_family = AF_UNIX;
+  strlcpy(sun.sun_path, path.get(), sizeof(sun.sun_path));
+  if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) != 0) {
+    // Nobody is serving this profile: start normally.
+    LOG("  connect: %s", strerror(errno));
+    close(fd);
+    return NS_ERROR_NOT_AVAILABLE;
+  }
+
+  uint32_t netLen = uint32_t(len);
+  if (!WriteAll(fd, reinterpret_cast<const char*>(&netLen), sizeof(netLen)) ||
+      !WriteAll(fd, commandLine.get(), len)) {
+    close(fd);
+    return NS_ERROR_FAILURE;
+  }
+
+  char reply[128];
+  size_t got = 0;
+  struct pollfd pfd = {fd, POLLIN, 0};
+  while (got < sizeof(reply) - 1 && poll(&pfd, 1, kReplyTimeoutMs) > 0) {
+    ssize_t n = read(fd, reply + got, sizeof(reply) - 1 - got);
+    if (n < 0 && errno == EINTR) continue;
+    if (n <= 0) break;
+    got += n;
+  }
+  close(fd);
+  reply[got] = '\0';
+  LOG("  reply: %s", reply);
+  return reply[0] == '2' ? NS_OK : NS_ERROR_FAILURE;
+}
