$NetBSD$

LISPBSD: hand URLs to the running instance over a Unix socket in
XDG_RUNTIME_DIR (the X11 remote needs an X server, the D-Bus one a
session bus; a plain Wayland session has neither).

--- toolkit/components/remote/nsRemoteService.cpp.orig
+++ toolkit/components/remote/nsRemoteService.cpp
@@ -8,8 +8,8 @@
 #    include "nsDBusRemoteServer.h"
 #    include "nsDBusRemoteClient.h"
 #  else
-#    include "nsGTKRemoteServer.h"
-#    include "nsXRemoteClient.h"
+#    include "nsSocketRemoteServer.h"
+#    include "nsSocketRemoteClient.h"
 #  endif
 #elif defined(XP_WIN)
 #  include "nsWinRemoteServer.h"
@@ -196,7 +196,7 @@
 #  if defined(MOZ_ENABLE_DBUS)
   client = MakeUnique<nsDBusRemoteClient>(mStartupToken);
 #  else
-  client = MakeUnique<nsXRemoteClient>(mStartupToken);
+  client = MakeUnique<nsSocketRemoteClient>(mStartupToken);
 #  endif
 #elif defined(XP_WIN)
   client = MakeUnique<nsWinRemoteClient>();
@@ -258,7 +258,7 @@
 #  if defined(MOZ_ENABLE_DBUS)
   mRemoteServer = MakeUnique<nsDBusRemoteServer>();
 #  else
-  mRemoteServer = MakeUnique<nsGTKRemoteServer>();
+  mRemoteServer = MakeUnique<nsSocketRemoteServer>();
 #  endif
 #elif defined(XP_WIN)
   mRemoteServer = MakeUnique<nsWinRemoteServer>();
