#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <string.h>
#include <glib.h>

#include "capture.h"
#include "compositing.h"

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

static void
test_compositing_false_without_manager (void)
{
  g_assert_false (screencast_compositing_active ());
}

static void
test_compositing_true_when_selection_owned (void)
{
  Window owner = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), 0, 0, 1, 1, 0, 0, 0);
  Atom sel = XInternAtom (dpy, "_NET_WM_CM_S0", False);

  XSetSelectionOwner (dpy, sel, owner, CurrentTime);
  XSync (dpy, False);
  g_assert_true (screencast_compositing_active ());

  XSetSelectionOwner (dpy, sel, None, CurrentTime);
  XSync (dpy, False);
  g_assert_false (screencast_compositing_active ());
}

static Window
make_window (int x, int y, int w, int h, unsigned long colour, const char *title)
{
  Window win = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), x, y, w, h, 0, 0, colour);

  /* Stand in for a compositor: keep the contents of obscured windows */
  XCompositeRedirectWindow (dpy, win, CompositeRedirectAutomatic);
  XChangeProperty (dpy, win, XInternAtom (dpy, "_NET_WM_NAME", False),
                   XInternAtom (dpy, "UTF8_STRING", False), 8, PropModeReplace,
                   (const unsigned char *) title, strlen (title));
  XMapWindow (dpy, win);
  XSync (dpy, False);

  return win;
}

static void
set_client_list (Window *wins, int n)
{
  XChangeProperty (dpy, DefaultRootWindow (dpy), XInternAtom (dpy, "_NET_CLIENT_LIST", False),
                   XA_WINDOW, 32, PropModeReplace, (const unsigned char *) wins, n);
  XSync (dpy, False);
}

static void
test_window_grab_of_obscured_window (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  ScreenCastSource *red = NULL;
  g_autofree guint8 *buf = NULL;
  Window wins[2];
  guint i;

  g_assert_no_error (error);

  wins[0] = make_window (50, 50, 200, 200, 0xff0000, "red");
  wins[1] = make_window (100, 100, 200, 200, 0x0000ff, "blue");
  set_client_list (wins, 2);

  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_WINDOW);
  g_assert_cmpuint (sources->len, ==, 2);
  for (i = 0; i < sources->len; i++)
    {
      ScreenCastSource *s = g_ptr_array_index (sources, i);

      g_assert_cmpint (s->type, ==, SCREENCAST_SOURCE_WINDOW);
      if (g_strcmp0 (s->name, "red") == 0)
        red = s;
    }
  g_assert_nonnull (red);
  g_assert_cmpint (red->width, ==, 200);
  g_assert_cmpint (red->height, ==, 200);

  buf = g_malloc0 (red->width * 4 * red->height);
  g_assert_true (screencast_capture_grab (c, red, FALSE, buf, red->width * 4, &error));
  g_assert_no_error (error);

  /* (100,100) in the red window is covered on screen by the blue one */
  g_assert_cmpuint (buf[100 * 800 + 100 * 4 + 0], ==, 0x00);
  g_assert_cmpuint (buf[100 * 800 + 100 * 4 + 2], ==, 0xff);

  screencast_capture_free (c);

  XDestroyWindow (dpy, wins[0]);
  XDestroyWindow (dpy, wins[1]);
  set_client_list (wins, 0);
}

static void
test_window_closed_gives_source_gone (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  g_autofree guint8 *buf = g_malloc0 (200 * 4 * 200);
  Window win = make_window (10, 10, 200, 200, 0x00ff00, "green");

  set_client_list (&win, 1);
  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_WINDOW);
  g_assert_cmpuint (sources->len, ==, 1);

  XDestroyWindow (dpy, win);
  XSync (dpy, False);

  g_assert_false (screencast_capture_grab (c, g_ptr_array_index (sources, 0), FALSE,
                                           buf, 200 * 4, &error));
  g_assert_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_SOURCE_GONE);

  screencast_capture_free (c);
  set_client_list (&win, 0);
}

static gboolean
region_differs (const guint8 *a, const guint8 *b, int stride, int x0, int y0, int n)
{
  int x, y;

  for (y = y0; y < y0 + n; y++)
    for (x = x0; x < x0 + n; x++)
      if (memcmp (a + y * stride + x * 4, b + y * stride + x * 4, 3) != 0)
        return TRUE;

  return FALSE;
}

static void
test_cursor_embedded_changes_pixels (void)
{
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *c = screencast_capture_x11_new (&error);
  g_autoptr(GPtrArray) sources = NULL;
  ScreenCastSource *s;
  gsize stride = 800 * 4;
  g_autofree guint8 *plain = g_malloc0 (stride * 600);
  g_autofree guint8 *with_cursor = g_malloc0 (stride * 600);
  int y, x;

  paint_root (ROOT_COLOUR);
  XWarpPointer (dpy, None, DefaultRootWindow (dpy), 0, 0, 0, 0, 100, 100);
  XSync (dpy, False);

  sources = screencast_capture_list_sources (c, SCREENCAST_SOURCE_MONITOR);
  s = g_ptr_array_index (sources, 0);

  g_assert_true (screencast_capture_grab (c, s, FALSE, plain, stride, &error));
  g_assert_true (screencast_capture_grab (c, s, TRUE, with_cursor, stride, &error));
  g_assert_no_error (error);

  /* Without the cursor the screen is the plain background everywhere */
  for (y = 0; y < 600; y += 50)
    for (x = 0; x < 800; x += 50)
      g_assert_cmpuint (plain[y * stride + x * 4 + 2], ==, 0x33);

  g_assert_true (region_differs (plain, with_cursor, stride, 84, 84, 40));

  screencast_capture_free (c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  dpy = XOpenDisplay (NULL);
  g_assert_nonnull (dpy);

  g_test_add_func ("/capture/monitor_grab", test_monitor_listed_and_grab_matches_root_colour);
  g_test_add_func ("/capture/compositing_false_without_manager", test_compositing_false_without_manager);
  g_test_add_func ("/capture/compositing_true_when_selection_owned", test_compositing_true_when_selection_owned);
  g_test_add_func ("/capture/window_grab_obscured", test_window_grab_of_obscured_window);
  g_test_add_func ("/capture/window_closed", test_window_closed_gives_source_gone);
  g_test_add_func ("/capture/cursor_embedded", test_cursor_embedded_changes_pixels);

  return g_test_run ();
}
