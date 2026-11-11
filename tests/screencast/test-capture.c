#include <X11/Xlib.h>
#include <glib.h>

#include "capture.h"

#define ROOT_COLOUR 0x336699

static Display *dpy;

static void
paint_root (unsigned long colour)
{
  Window root = DefaultRootWindow (dpy);

  XSetWindowBackground (dpy, root, colour);
  XClearWindow (dpy, root);
  XSync (dpy, False);
}

static void
test_monitor_listed_and_grab_matches_root_colour (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  ScreenCastSource *s;
  g_autofree guint8 *buf = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (c);

  paint_root (ROOT_COLOUR);

  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_MONITOR);
  g_assert_cmpuint (sources->len, ==, 1);
  s = g_ptr_array_index (sources, 0);
  g_assert_cmpint (s->type, ==, SCREENCAST_SOURCE_MONITOR);
  g_assert_cmpint (s->width, ==, 800);
  g_assert_cmpint (s->height, ==, 600);

  buf = g_malloc0 (s->width * 4 * s->height);
  g_assert_true (screencast_capture_grab (c, s, FALSE, buf, s->width * 4, &error));
  g_assert_no_error (error);

  /* BGRx: byte order is B, G, R */
  g_assert_cmpuint (buf[10 * s->width * 4 + 10 * 4 + 0], ==, 0x99);
  g_assert_cmpuint (buf[10 * s->width * 4 + 10 * 4 + 1], ==, 0x66);
  g_assert_cmpuint (buf[10 * s->width * 4 + 10 * 4 + 2], ==, 0x33);

  screencast_capture_free (c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  dpy = XOpenDisplay (NULL);
  g_assert_nonnull (dpy);

  g_test_add_func ("/capture/monitor_grab", test_monitor_listed_and_grab_matches_root_colour);

  return g_test_run ();
}
