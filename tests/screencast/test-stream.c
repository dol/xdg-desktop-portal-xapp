#include <dirent.h>
#include <signal.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <gio/gio.h>
#include <glib.h>

#include "capture.h"
#include "stream.h"

static Display *dpy;

static int
count_fds (void)
{
  DIR *d = opendir ("/proc/self/fd");
  int n = 0;

  while (readdir (d))
    n++;
  closedir (d);

  return n;
}

typedef struct
{
  GMainLoop *loop;
  int status;
  gboolean done;
} Wait;

static void
on_gst_exit (GPid pid, gint status, gpointer data)
{
  Wait *w = data;

  w->status = status;
  w->done = TRUE;
  g_spawn_close_pid (pid);
  g_main_loop_quit (w->loop);
}

static gboolean
on_timeout (gpointer data)
{
  Wait *w = data;

  g_main_loop_quit (w->loop);
  return G_SOURCE_REMOVE;
}

/* Pulls `n` frames of the given size from the node with GStreamer while the
 * main loop keeps the stream producing. */
static gboolean
pull_frames (guint32 node, int width, int height, int n)
{
  /* Streams are created and destroyed one at a time, so the name is unique */
  char target[] = "target-object=xapp-screencast";
  g_autofree char *num = g_strdup_printf ("num-buffers=%d", n);
  g_autofree char *caps = g_strdup_printf ("video/x-raw,width=%d,height=%d", width, height);
  /* Browsers set media.type up front; pipewiresrc only does after negotiation,
   * which is too late for WirePlumber to link it. */
  char *argv[] = { "gst-launch-1.0", "pipewiresrc", target,
                   "stream-properties=props,media.type=Video,media.category=Capture,media.role=Screen",
                   num, "!", "videoconvert", "!", caps, "!", "fakesink", NULL };
  Wait w = { g_main_loop_new (NULL, FALSE), -1, FALSE };
  GPid pid;
  guint t;

  g_assert_true (g_spawn_async (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, NULL));
  g_child_watch_add (pid, on_gst_exit, &w);
  t = g_timeout_add_seconds (15, on_timeout, &w);

  g_main_loop_run (w.loop);

  if (!w.done)
    kill (pid, SIGKILL);
  else
    g_source_remove (t);

  g_main_loop_unref (w.loop);

  return w.done && g_spawn_check_wait_status (w.status, NULL);
}

static void
test_stream_delivers_frames (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_MONITOR);
  ScreenCastStream *stream;

  g_assert_no_error (error);
  stream = screencast_stream_new (c, g_ptr_array_index (sources, 0), FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stream);
  g_assert_cmpuint (screencast_stream_get_node_id (stream), !=, (guint32) -1);

  g_assert_true (pull_frames (screencast_stream_get_node_id (stream), 800, 600, 5));

  screencast_stream_destroy (stream);
  screencast_capture_free (c);
}

static void
create_and_destroy_stream (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_MONITOR);
  ScreenCastStream *stream = screencast_stream_new (c, g_ptr_array_index (sources, 0), FALSE, &error);

  g_assert_no_error (error);
  screencast_stream_destroy (stream);
  screencast_capture_free (c);
}

static void
test_stream_destroy_releases_fds (void)
{
  int before, after, i;

  /* Warm up one-time allocations (pw_init, X extension lookups) */
  create_and_destroy_stream ();

  before = count_fds ();
  for (i = 0; i < 5; i++)
    create_and_destroy_stream ();
  after = count_fds ();

  g_assert_cmpint (after, ==, before);
}

typedef struct
{
  gboolean closed;
  GMainLoop *loop;
} Closed;

static void
on_closed (ScreenCastStream *stream, gpointer data)
{
  Closed *cl = data;

  cl->closed = TRUE;
  g_main_loop_quit (cl->loop);
}

static void
test_stream_closes_when_window_destroyed (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  Closed cl = { FALSE, g_main_loop_new (NULL, FALSE) };
  Window win = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), 10, 10, 200, 200, 0, 0, 0x00ff00);
  ScreenCastStream *stream;
  Window list[1] = { win };

  XCompositeRedirectWindow (dpy, win, CompositeRedirectAutomatic);
  XMapWindow (dpy, win);
  XChangeProperty (dpy, DefaultRootWindow (dpy), XInternAtom (dpy, "_NET_CLIENT_LIST", False),
                   XA_WINDOW, 32, PropModeReplace, (unsigned char *) list, 1);
  XSync (dpy, False);

  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_WINDOW);
  g_assert_cmpuint (sources->len, ==, 1);
  stream = screencast_stream_new (c, g_ptr_array_index (sources, 0), FALSE, &error);
  g_assert_no_error (error);
  screencast_stream_set_closed_callback (stream, on_closed, &cl);

  XDestroyWindow (dpy, win);
  XSync (dpy, False);

  g_timeout_add_seconds (5, on_timeout, &(Wait){ cl.loop, 0, FALSE });
  g_main_loop_run (cl.loop);
  g_assert_true (cl.closed);

  screencast_stream_destroy (stream);
  screencast_capture_free (c);
  g_main_loop_unref (cl.loop);
}

typedef struct
{
  Window win;
} Resizer;

static gboolean
do_resize (gpointer data)
{
  Resizer *r = data;

  XResizeWindow (dpy, r->win, 320, 240);
  XSync (dpy, False);
  return G_SOURCE_REMOVE;
}

static void
test_stream_survives_window_resize (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  Closed cl = { FALSE, g_main_loop_new (NULL, FALSE) };
  Window win = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), 10, 10, 200, 200, 0, 0, 0x00ff00);
  ScreenCastStream *stream;
  Window list[1] = { win };
  Resizer r = { win };

  XCompositeRedirectWindow (dpy, win, CompositeRedirectAutomatic);
  XMapWindow (dpy, win);
  XChangeProperty (dpy, DefaultRootWindow (dpy), XInternAtom (dpy, "_NET_CLIENT_LIST", False),
                   XA_WINDOW, 32, PropModeReplace, (unsigned char *) list, 1);
  XSync (dpy, False);

  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_WINDOW);
  stream = screencast_stream_new (c, g_ptr_array_index (sources, 0), FALSE, &error);
  g_assert_no_error (error);
  screencast_stream_set_closed_callback (stream, on_closed, &cl);

  g_timeout_add (500, do_resize, &r);

  /* The consumer asks for the new size only: it must be renegotiated, and
   * the stream must not have been closed by the resize. */
  g_timeout_add_seconds (2, on_timeout, &(Wait){ cl.loop, 0, FALSE });
  g_main_loop_run (cl.loop);
  g_assert_false (cl.closed);
  g_assert_true (pull_frames (screencast_stream_get_node_id (stream), 320, 240, 5));

  screencast_stream_destroy (stream);
  screencast_capture_free (c);
  XDestroyWindow (dpy, win);
  g_main_loop_unref (cl.loop);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  dpy = XOpenDisplay (NULL);
  g_assert_nonnull (dpy);

  g_test_add_func ("/stream/delivers_frames", test_stream_delivers_frames);
  g_test_add_func ("/stream/destroy_releases_fds", test_stream_destroy_releases_fds);
  g_test_add_func ("/stream/closes_when_window_destroyed", test_stream_closes_when_window_destroyed);
  g_test_add_func ("/stream/survives_window_resize", test_stream_survives_window_resize);

  return g_test_run ();
}
