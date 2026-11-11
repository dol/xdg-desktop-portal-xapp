#include <gtk/gtk.h>

#include "capture.h"
#include "screencastdialog.h"

static GPtrArray *
make_sources (void)
{
  GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) screencast_source_free);
  struct { guint32 id; ScreenCastSourceType type; const char *name; int w, h; } defs[] = {
    { 100, SCREENCAST_SOURCE_MONITOR, "HDMI-1", 1920, 1080 },
    { 101, SCREENCAST_SOURCE_MONITOR, "DP-1", 2560, 1440 },
    { 200, SCREENCAST_SOURCE_WINDOW, "Terminal", 800, 600 },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (defs); i++)
    {
      ScreenCastSource *s = g_new0 (ScreenCastSource, 1);

      s->id = defs[i].id;
      s->type = defs[i].type;
      s->name = g_strdup (defs[i].name);
      s->width = defs[i].w;
      s->height = defs[i].h;
      g_ptr_array_add (a, s);
    }

  return a;
}

static GtkWidget *
find_dialog (void)
{
  GList *l, *tops = gtk_window_list_toplevels ();
  GtkWidget *found = NULL;

  for (l = tops; l; l = l->next)
    if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data))
      found = l->data;
  g_list_free (tops);

  return found;
}

static GtkWidget *
find_list (GtkWidget *w)
{
  GtkWidget *found = NULL;

  if (GTK_IS_LIST_BOX (w))
    return w;

  if (GTK_IS_CONTAINER (w))
    {
      GList *l, *children = gtk_container_get_children (GTK_CONTAINER (w));

      for (l = children; l && !found; l = l->next)
        found = find_list (l->data);
      g_list_free (children);
    }

  return found;
}

typedef struct
{
  GArray *ids;
  GError *error;
  gboolean done;
} Result;

static void
on_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Result *r = data;

  r->ids = screencast_dialog_run_finish (res, &r->error);
  r->done = TRUE;
}

static void
spin_until (gboolean *flag)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;

  while (!*flag && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
}

static GtkWidget *
start_dialog_with (GPtrArray *sources, ScreenCastCapture *capture, gboolean multiple, GCancellable *c, Result *r)
{
  GtkWidget *dialog;
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;

  screencast_dialog_run_async (NULL, sources, capture, multiple, c, on_done, r);
  while (!(dialog = find_dialog ()) && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
  g_assert_nonnull (dialog);

  return dialog;
}

static GtkWidget *
start_dialog (GPtrArray *sources, gboolean multiple, GCancellable *c, Result *r)
{
  return start_dialog_with (sources, NULL, multiple, c, r);
}

/* A capture that returns solid red for every source */
static gboolean
red_grab (ScreenCastCapture *c, const ScreenCastSource *s, gboolean embed_cursor,
          guint8 *dst, gsize stride, GError **error)
{
  int x, y;

  for (y = 0; y < s->height; y++)
    for (x = 0; x < s->width; x++)
      {
        guint8 *px = dst + y * stride + x * 4;

        px[0] = 0x00; px[1] = 0x00; px[2] = 0xff; px[3] = 0xff; /* BGRx */
      }

  return TRUE;
}

static void
collect_thumbnails (GtkWidget *w, GPtrArray *out)
{
  if (GTK_IS_IMAGE (w) && g_strcmp0 (gtk_widget_get_name (w), "screencast-thumbnail") == 0)
    g_ptr_array_add (out, w);

  if (GTK_IS_CONTAINER (w))
    {
      GList *l, *children = gtk_container_get_children (GTK_CONTAINER (w));

      for (l = children; l; l = l->next)
        collect_thumbnails (l->data, out);
      g_list_free (children);
    }
}

static void
test_rows_show_scaled_thumbnails (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  g_autoptr(GPtrArray) thumbs = g_ptr_array_new ();
  ScreenCastCapture fake = { .grab = red_grab };
  Result r = { 0 };
  GtkWidget *dialog = start_dialog_with (sources, &fake, FALSE, NULL, &r);
  int expected_w[] = { 160, 160, 120 };  /* 16:9, 16:9, 4:3 fitted into 160x90 */
  guint i;

  collect_thumbnails (dialog, thumbs);
  g_assert_cmpuint (thumbs->len, ==, 3);

  for (i = 0; i < thumbs->len; i++)
    {
      GdkPixbuf *pb = gtk_image_get_pixbuf (GTK_IMAGE (g_ptr_array_index (thumbs, i)));
      const guint8 *center;

      g_assert_nonnull (pb);
      g_assert_cmpint (gdk_pixbuf_get_width (pb), ==, expected_w[i]);
      g_assert_cmpint (gdk_pixbuf_get_height (pb), ==, 90);

      center = gdk_pixbuf_get_pixels (pb) + 45 * gdk_pixbuf_get_rowstride (pb) +
               (expected_w[i] / 2) * gdk_pixbuf_get_n_channels (pb);
      g_assert_cmpuint (center[0], ==, 0xff);  /* R */
      g_assert_cmpuint (center[1], ==, 0x00);
      g_assert_cmpuint (center[2], ==, 0x00);
    }

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  spin_until (&r.done);
  g_error_free (r.error);
}

static gboolean
failing_grab (ScreenCastCapture *c, const ScreenCastSource *s, gboolean embed_cursor,
              guint8 *dst, gsize stride, GError **error)
{
  g_set_error_literal (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_SOURCE_GONE, "gone");
  return FALSE;
}

static void
test_failed_grab_still_lists_sources_without_thumbnail (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  g_autoptr(GPtrArray) thumbs = g_ptr_array_new ();
  ScreenCastCapture fake = { .grab = failing_grab };
  Result r = { 0 };
  GtkWidget *dialog = start_dialog_with (sources, &fake, FALSE, NULL, &r);
  GtkWidget *list = find_list (dialog);

  collect_thumbnails (dialog, thumbs);
  g_assert_cmpuint (thumbs->len, ==, 0);
  g_assert_nonnull (gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 2));

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  spin_until (&r.done);
  g_error_free (r.error);
}

static void
test_default_size_fits_previews_within_screen (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  Result r = { 0 };
  GtkWidget *dialog = start_dialog (sources, FALSE, NULL, &r);
  GdkRectangle area;
  int w, h;

  gtk_window_get_default_size (GTK_WINDOW (dialog), &w, &h);
  gdk_monitor_get_workarea (gdk_display_get_monitor (gdk_display_get_default (), 0), &area);

  /* A 160px preview plus the text needs more than the old 420px */
  g_assert_cmpint (w, >=, 560);
  g_assert_cmpint (w, <=, area.width);
  g_assert_cmpint (h, <=, area.height);
  g_assert_cmpint (h, >=, 300);

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  spin_until (&r.done);
  g_error_free (r.error);
}

static void
test_cancel_gives_cancelled_error (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  Result r = { 0 };
  GtkWidget *dialog = start_dialog (sources, FALSE, NULL, &r);

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  spin_until (&r.done);

  g_assert_null (r.ids);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_error_free (r.error);
}

static void
test_single_select_returns_one_id (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  Result r = { 0 };
  GtkWidget *dialog = start_dialog (sources, FALSE, NULL, &r);
  GtkWidget *list = find_list (dialog);
  GtkWidget *share = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  g_assert_nonnull (list);
  g_assert_false (gtk_widget_get_sensitive (share));

  gtk_list_box_select_row (GTK_LIST_BOX (list), gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 1));
  g_assert_true (gtk_widget_get_sensitive (share));

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  spin_until (&r.done);

  g_assert_no_error (r.error);
  g_assert_cmpuint (r.ids->len, ==, 1);
  g_assert_cmpuint (g_array_index (r.ids, guint32, 0), ==, 101);
  g_array_unref (r.ids);
}

static void
test_multiple_select_returns_all_selected (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  Result r = { 0 };
  GtkWidget *dialog = start_dialog (sources, TRUE, NULL, &r);
  GtkWidget *list = find_list (dialog);

  gtk_list_box_select_row (GTK_LIST_BOX (list), gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0));
  gtk_list_box_select_row (GTK_LIST_BOX (list), gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 2));

  gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  spin_until (&r.done);

  g_assert_no_error (r.error);
  g_assert_cmpuint (r.ids->len, ==, 2);
  g_assert_cmpuint (g_array_index (r.ids, guint32, 0), ==, 100);
  g_assert_cmpuint (g_array_index (r.ids, guint32, 1), ==, 200);
  g_array_unref (r.ids);
}

static void
test_cancellable_dismisses_dialog (void)
{
  g_autoptr(GPtrArray) sources = make_sources ();
  g_autoptr(GCancellable) c = g_cancellable_new ();
  Result r = { 0 };

  start_dialog (sources, FALSE, c, &r);
  g_cancellable_cancel (c);
  spin_until (&r.done);

  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_error_free (r.error);
  g_assert_null (find_dialog ());
}

int
main (int argc, char **argv)
{
  g_assert_true (gtk_init_check (&argc, &argv));
  g_test_init (&argc, &argv, NULL);
  g_unsetenv ("XAPP_SCREENCAST_TEST_AUTOPICK");

  g_test_add_func ("/dialog/thumbnails", test_rows_show_scaled_thumbnails);
  g_test_add_func ("/dialog/thumbnail_grab_fails", test_failed_grab_still_lists_sources_without_thumbnail);
  g_test_add_func ("/dialog/default_size", test_default_size_fits_previews_within_screen);
  g_test_add_func ("/dialog/cancel", test_cancel_gives_cancelled_error);
  g_test_add_func ("/dialog/single_select", test_single_select_returns_one_id);
  g_test_add_func ("/dialog/multiple_select", test_multiple_select_returns_all_selected);
  g_test_add_func ("/dialog/cancellable", test_cancellable_dismisses_dialog);

  return g_test_run ();
}
