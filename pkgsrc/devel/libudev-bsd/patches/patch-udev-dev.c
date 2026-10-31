$NetBSD$

The kernel posts "drm-hotplug" for dri/cardN when connectors change (a
monitor plugged in): report it as a "change" of the card, which carries
HOTPLUG=1, as udev does on Linux, so compositors rescan their connectors.

--- udev-dev.c.orig
+++ udev-dev.c
@@ -413,6 +413,8 @@
 		action = UD_ACTION_ADD;
 	else if (strcmp(msg.event, NDEVD_DETACH_EVENT) == 0)
 		action = UD_ACTION_REMOVE;
+	else if (strcmp(msg.event, "drm-hotplug") == 0)
+		action = UD_ACTION_HOTPLUG;	/* device "dri/cardN" */
 	else
         return (UD_ACTION_NONE);
 
