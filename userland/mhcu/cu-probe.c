/*
 * cu-probe: a GTK3 window that prints the pointer input it receives, for
 * testing mhcu (docs/computer-use.org).  The left half is a drag source,
 * the right half a drop target; one line per event on stdout:
 *   press BUTTON X Y MODS CLICKS | release BUTTON X Y MODS
 *   motion X Y MODS | scroll DX DY SOURCE MODS
 *   dnd-begin | dnd-drop TEXT
 * cc -O2 -o cu-probe cu-probe.c $(pkg-config --cflags --libs gtk+-3.0)
 */
#include <gtk/gtk.h>
#include <stdio.h>

static const char *
mods(guint state)
{
	static char buf[64];

	snprintf(buf, sizeof(buf), "%s%s%s%s%s",
	    state & GDK_SHIFT_MASK ? "shift," : "",
	    state & GDK_CONTROL_MASK ? "ctrl," : "",
	    state & GDK_MOD1_MASK ? "alt," : "",
	    state & GDK_SUPER_MASK ? "super," : "",
	    state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK |
	    GDK_SUPER_MASK) ? "" : "none");
	return buf;
}

static gboolean
on_button(GtkWidget *w, GdkEventButton *e, gpointer data)
{
	int clicks = e->type == GDK_3BUTTON_PRESS ? 3 :
	    e->type == GDK_2BUTTON_PRESS ? 2 : 1;

	if (e->type == GDK_BUTTON_RELEASE)
		printf("release %u %.0f %.0f %s\n", e->button, e->x_root,
		    e->y_root, mods(e->state));
	else
		printf("press %u %.0f %.0f %s %d\n", e->button, e->x_root,
		    e->y_root, mods(e->state), clicks);
	fflush(stdout);
	return FALSE;
}

static gboolean
on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
	printf("motion %.0f %.0f %s\n", e->x_root, e->y_root, mods(e->state));
	fflush(stdout);
	return FALSE;
}

static gboolean
on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
	GdkInputSource src = gdk_device_get_source(
	    gdk_event_get_source_device((GdkEvent *)e));
	double dx = 0, dy = 0;

	if (e->direction == GDK_SCROLL_SMOOTH)
		gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy);
	else
		dy = e->direction == GDK_SCROLL_DOWN ? 1 :
		    e->direction == GDK_SCROLL_UP ? -1 : 0;
	printf("scroll %.2f %.2f %s %s\n", dx, dy,
	    src == GDK_SOURCE_MOUSE ? "wheel" :
	    src == GDK_SOURCE_TOUCHPAD ? "touchpad" :
	    src == GDK_SOURCE_TRACKPOINT ? "trackpoint" : "other",
	    mods(e->state));
	fflush(stdout);
	return FALSE;
}

static void
on_drag_begin(GtkWidget *w, GdkDragContext *c, gpointer data)
{
	printf("dnd-begin\n");
	fflush(stdout);
}

static void
on_drag_get(GtkWidget *w, GdkDragContext *c, GtkSelectionData *sel,
    guint info, guint time, gpointer data)
{
	gtk_selection_data_set_text(sel, "cu-probe-payload", -1);
}

static void
on_drag_received(GtkWidget *w, GdkDragContext *c, int x, int y,
    GtkSelectionData *sel, guint info, guint time, gpointer data)
{
	guchar *text = gtk_selection_data_get_text(sel);

	printf("dnd-drop %s\n", text ? (char *)text : "(none)");
	fflush(stdout);
	g_free(text);
	gtk_drag_finish(c, TRUE, FALSE, time);
}

int
main(int argc, char **argv)
{
	GtkTargetEntry target = { "text/plain", 0, 0 };
	GtkWidget *win, *box, *src, *dst;

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "cu-probe");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
	src = gtk_event_box_new();
	dst = gtk_event_box_new();
	gtk_container_add(GTK_CONTAINER(src), gtk_label_new("drag from here"));
	gtk_container_add(GTK_CONTAINER(dst), gtk_label_new("drop here"));
	gtk_box_pack_start(GTK_BOX(box), src, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), dst, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);

	gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK |
	    GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
	    GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
	g_signal_connect(win, "button-press-event", G_CALLBACK(on_button), NULL);
	g_signal_connect(win, "button-release-event", G_CALLBACK(on_button),
	    NULL);
	g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_motion), NULL);
	g_signal_connect(win, "scroll-event", G_CALLBACK(on_scroll), NULL);

	gtk_drag_source_set(src, GDK_BUTTON1_MASK, &target, 1, GDK_ACTION_COPY);
	g_signal_connect(src, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
	g_signal_connect(src, "drag-data-get", G_CALLBACK(on_drag_get), NULL);
	gtk_drag_dest_set(dst, GTK_DEST_DEFAULT_ALL, &target, 1,
	    GDK_ACTION_COPY);
	g_signal_connect(dst, "drag-data-received",
	    G_CALLBACK(on_drag_received), NULL);

	gtk_widget_show_all(win);
	gtk_main();
	return 0;
}
