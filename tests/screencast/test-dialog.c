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
start_dialog (GPtrArray *sources, gboolean multiple, GCancellable *c, Result *r)
{
  GtkWidget *dialog;
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;

  screencast_dialog_run_async (NULL, sources, multiple, c, on_done, r);
  while (!(dialog = find_dialog ()) && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, FALSE);
  g_assert_nonnull (dialog);

  return dialog;
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

  g_test_add_func ("/dialog/cancel", test_cancel_gives_cancelled_error);
  g_test_add_func ("/dialog/single_select", test_single_select_returns_one_id);
  g_test_add_func ("/dialog/multiple_select", test_multiple_select_returns_all_selected);
  g_test_add_func ("/dialog/cancellable", test_cancellable_dismisses_dialog);

  return g_test_run ();
}
