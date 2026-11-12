/*
 * Copyright © 2026 xdg-desktop-portal-xapp contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <string.h>

#include <glib/gi18n.h>
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#include "screencastdialog.h"

#ifdef XAPP_SCREENCAST_TEST_HOOKS
static gboolean
autopick_idle (gpointer data)
{
  GTask *task = data;
  GPtrArray *sources = g_task_get_task_data (task);
  g_autofree char *flag = g_build_filename (g_get_user_runtime_dir (), "xapp-autopick-cancel", NULL);

  if (g_file_test (flag, G_FILE_TEST_EXISTS) || sources->len == 0)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
  else
    {
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
      guint32 id = ((ScreenCastSource *) g_ptr_array_index (sources, 0))->id;

      g_array_append_val (ids, id);
      g_task_return_pointer (task, ids, (GDestroyNotify) g_array_unref);
    }

  g_object_unref (task);
  return G_SOURCE_REMOVE;
}
#endif

typedef struct
{
  GtkWidget *dialog;
  GtkWidget *list;
  GtkWidget *share_button;
  GtkWidget *search;
  GPtrArray *cards;
  guint next_card;
  guint loader_id;
  ScreenCastCapture *capture;
  GtkWidget *fake_parent;
  ExternalWindow *external_parent;
  GCancellable *cancellable;
  gulong cancelled_id;
  GTask *task;
} Picker;

static void
picker_free (Picker *p)
{
  if (p->cancelled_id)
    g_cancellable_disconnect (p->cancellable, p->cancelled_id);

  gtk_widget_destroy (p->dialog);
  gtk_widget_destroy (p->fake_parent);
  g_object_unref (p->fake_parent);
  g_clear_object (&p->external_parent);
  g_clear_object (&p->cancellable);
  if (p->loader_id)
    g_source_remove (p->loader_id);
  g_ptr_array_unref (p->cards);
  g_object_unref (p->task);
  g_free (p);
}

static void
on_response (GtkDialog *dialog, int response, Picker *p)
{
  if (response == GTK_RESPONSE_ACCEPT)
    {
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
      GList *rows = gtk_flow_box_get_selected_children (GTK_FLOW_BOX (p->list));
      GList *l;

      for (l = rows; l; l = l->next)
        {
          guint32 id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (l->data), "source-id"));

          g_array_append_val (ids, id);
        }
      g_list_free (rows);

      g_task_return_pointer (p->task, ids, (GDestroyNotify) g_array_unref);
    }
  else
    {
      g_task_return_new_error (p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    }

  picker_free (p);
}

static void
on_cancelled (GCancellable *cancellable, Picker *p)
{
  /* Disconnecting from inside the handler would deadlock on the
   * cancellable's lock; it only ever fires once. */
  p->cancelled_id = 0;
  gtk_dialog_response (GTK_DIALOG (p->dialog), GTK_RESPONSE_CANCEL);
}

static GdkPixbuf *thumbnail_new (ScreenCastCapture *capture, const ScreenCastSource *source);

static gboolean
card_matches (GtkFlowBoxChild *child, Picker *p)
{
  const char *needle = gtk_entry_get_text (GTK_ENTRY (p->search));
  const char *haystack = g_object_get_data (G_OBJECT (child), "search-text");
  g_autofree char *folded = NULL;

  if (!*needle)
    return TRUE;

  folded = g_utf8_casefold (needle, -1);
  return haystack && strstr (haystack, folded) != NULL;
}

static gboolean
filter_func (GtkFlowBoxChild *child, gpointer data)
{
  return card_matches (child, data);
}

static void
on_search_changed (GtkEditable *entry, Picker *p)
{
  GList *selected, *l;

  gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (p->list));

  /* A hidden card must never stay selected: Share would send an invisible source */
  selected = gtk_flow_box_get_selected_children (GTK_FLOW_BOX (p->list));
  for (l = selected; l; l = l->next)
    if (!card_matches (l->data, p))
      gtk_flow_box_unselect_child (GTK_FLOW_BOX (p->list), l->data);
  g_list_free (selected);
}

/* One thumbnail per idle iteration, so opening never waits for every grab */
static gboolean
load_next_thumbnail (gpointer data)
{
  Picker *p = data;
  GtkWidget *card, *preview;
  g_autoptr(GdkPixbuf) pixbuf = NULL;

  if (p->next_card >= p->cards->len)
    {
      p->loader_id = 0;
      return G_SOURCE_REMOVE;
    }

  card = g_ptr_array_index (p->cards, p->next_card++);
  preview = g_object_get_data (G_OBJECT (card), "preview");
  pixbuf = thumbnail_new (p->capture, g_object_get_data (G_OBJECT (card), "source"));

  if (pixbuf)
    {
      gtk_image_set_from_pixbuf (GTK_IMAGE (preview), pixbuf);
      gtk_widget_set_name (preview, "screencast-thumbnail");
    }

  return G_SOURCE_CONTINUE;
}

static void
on_selection_changed (GtkFlowBox *box, Picker *p)
{
  GList *selected = gtk_flow_box_get_selected_children (box);

  gtk_widget_set_sensitive (p->share_button, selected != NULL);
  g_list_free (selected);
}

static void
on_child_activated (GtkFlowBox *box, GtkFlowBoxChild *child, Picker *p)
{
  /* Double-click shares straight away when only one thing can be picked */
  if (gtk_flow_box_get_selection_mode (box) == GTK_SELECTION_SINGLE)
    gtk_dialog_response (GTK_DIALOG (p->dialog), GTK_RESPONSE_ACCEPT);
}

#define THUMBNAIL_WIDTH 240
#define THUMBNAIL_HEIGHT 135
#define APP_ICON_SIZE 32

/* Grabs `source` once and fits it into THUMBNAIL_WIDTH x THUMBNAIL_HEIGHT.
 * NULL when there is nothing to show (no capture, or the source is gone). */
static GdkPixbuf *
thumbnail_new (ScreenCastCapture *capture, const ScreenCastSource *source)
{
  gsize stride = (gsize) source->width * 4;
  g_autofree guint8 *data = NULL;
  cairo_surface_t *full, *thumb;
  GdkPixbuf *pixbuf;
  cairo_t *cr;
  double scale;
  int w, h;

  if (!capture || source->width <= 0 || source->height <= 0)
    return NULL;

  data = g_try_malloc (stride * source->height);
  if (!data || !screencast_capture_grab (capture, source, FALSE, data, stride, NULL))
    return NULL;

  /* Never enlarge: small windows keep their size */
  scale = MIN (1.0, MIN ((double) THUMBNAIL_WIDTH / source->width,
                         (double) THUMBNAIL_HEIGHT / source->height));
  w = MAX (1, (int) (source->width * scale + 0.5));
  h = MAX (1, (int) (source->height * scale + 0.5));

  full = cairo_image_surface_create_for_data (data, CAIRO_FORMAT_RGB24,
                                              source->width, source->height, stride);
  thumb = cairo_image_surface_create (CAIRO_FORMAT_RGB24, w, h);

  cr = cairo_create (thumb);
  cairo_scale (cr, (double) w / source->width, (double) h / source->height);
  cairo_set_source_surface (cr, full, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_destroy (cr);

  pixbuf = gdk_pixbuf_get_from_surface (thumb, 0, 0, w, h);

  cairo_surface_destroy (full);
  cairo_surface_destroy (thumb);

  return pixbuf;
}

/* The icon of the application owning a window: the desktop entry whose
 * StartupWMClass or id matches the window's WM_CLASS. */
static GIcon *
app_icon_for_class (const char *wm_class)
{
  GList *apps, *l;
  GIcon *icon = NULL;
  g_autofree char *lower = NULL;

  if (!wm_class || !*wm_class)
    return NULL;

  lower = g_ascii_strdown (wm_class, -1);
  apps = g_app_info_get_all ();

  for (l = apps; l && !icon; l = l->next)
    {
      GAppInfo *info = l->data;
      const char *startup_class = G_IS_DESKTOP_APP_INFO (info) ?
        g_desktop_app_info_get_startup_wm_class (G_DESKTOP_APP_INFO (info)) : NULL;
      const char *id = g_app_info_get_id (info);
      g_autofree char *id_lower = id ? g_ascii_strdown (id, -1) : NULL;

      if ((startup_class && g_ascii_strcasecmp (startup_class, wm_class) == 0) ||
          (id_lower && (g_str_equal (id_lower, lower) ||
                        (g_str_has_suffix (id_lower, ".desktop") &&
                         strlen (id_lower) - 8 == strlen (lower) &&
                         g_str_has_prefix (id_lower, lower)))))
        {
          GIcon *app_icon = g_app_info_get_icon (info);

          if (app_icon)
            icon = g_object_ref (app_icon);
        }
    }

  g_list_free_full (apps, g_object_unref);

  return icon;
}

static GtkWidget *
card_new (const ScreenCastSource *source)
{
  GtkWidget *card = gtk_flow_box_child_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  /* Icon themes differ: list fallbacks so a missing name never shows as an error icon */
  static const char *monitor_icons[] = { "video-display-symbolic", "video-display", "computer-symbolic", NULL };
  static const char *window_icons[] = { "window-symbolic", "preferences-system-windows-symbolic",
                                        "application-x-executable-symbolic", "application-x-executable", NULL };
  g_autoptr(GIcon) app_icon = source->type == SCREENCAST_SOURCE_WINDOW ?
    app_icon_for_class (source->app_id) : NULL;
  g_autoptr(GIcon) fallback = g_themed_icon_new_from_names ((char **) (source->type == SCREENCAST_SOURCE_MONITOR ?
                                                                       monitor_icons : window_icons), -1);
  g_autofree char *title = NULL;
  GtkWidget *icon, *label, *preview;

  icon = gtk_image_new_from_gicon (app_icon ? app_icon : fallback, GTK_ICON_SIZE_DND);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), APP_ICON_SIZE);
  gtk_widget_set_name (icon, "screencast-app-icon");

  if (source->type == SCREENCAST_SOURCE_MONITOR)
    title = g_strdup_printf ("%s (%d × %d)", source->name && *source->name ? source->name : _("Screen"),
                             source->width, source->height);
  else
    title = g_strdup (source->name && *source->name ? source->name : _("Untitled"));

  label = gtk_label_new (title);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 24);
  gtk_widget_set_tooltip_text (label, title);

  /* The real preview is filled in later, one card at a time */
  preview = gtk_image_new_from_gicon (fallback, GTK_ICON_SIZE_DIALOG);
  gtk_widget_set_size_request (preview, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);

  gtk_box_pack_start (GTK_BOX (header), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (header), label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (box), header, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), preview, FALSE, FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (box), 10);
  gtk_container_add (GTK_CONTAINER (card), box);
  gtk_widget_show_all (card);

  g_object_set_data (G_OBJECT (card), "source-id", GUINT_TO_POINTER (source->id));
  g_object_set_data (G_OBJECT (card), "preview", preview);
  g_object_set_data (G_OBJECT (card), "source", (gpointer) source);
  {
    g_autofree char *haystack = g_strconcat (title, " ", source->app_id ? source->app_id : "", NULL);

    g_object_set_data_full (G_OBJECT (card), "search-text", g_utf8_casefold (haystack, -1), g_free);
  }

  return card;
}

void
screencast_dialog_run_async (ExternalWindow *external_parent,
                             GPtrArray *sources,
                             ScreenCastCapture *capture,
                             gboolean multiple,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  GdkDisplay *display;
  GtkWidget *scrolled, *content, *cancel_button;
  Picker *p;
  guint i;

  g_task_set_task_data (task, g_ptr_array_ref (sources), (GDestroyNotify) g_ptr_array_unref);

#ifdef XAPP_SCREENCAST_TEST_HOOKS
  /* Skips the consent dialog. Only compiled with -Dscreencast_test_hooks=true,
   * which release builds must never use. */
  if (g_getenv ("XAPP_SCREENCAST_TEST_AUTOPICK"))
    {
      g_idle_add (autopick_idle, task);
      return;
    }
#endif

  p = g_new0 (Picker, 1);
  p->task = task;
  p->capture = capture;
  p->cards = g_ptr_array_new ();
  p->external_parent = external_parent ? g_object_ref (external_parent) : NULL;
  p->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  display = external_parent ? external_window_get_display (external_parent) : gdk_display_get_default ();
  p->fake_parent = g_object_new (GTK_TYPE_WINDOW,
                                 "type", GTK_WINDOW_TOPLEVEL,
                                 "screen", gdk_display_get_default_screen (display),
                                 NULL);
  g_object_ref_sink (p->fake_parent);

  p->dialog = gtk_dialog_new ();
  gtk_window_set_title (GTK_WINDOW (p->dialog), _("Share Your Screen"));
  {
    /* Two cards with 240px previews side by side, but never taller than the screen */
    GdkMonitor *monitor = gdk_display_get_primary_monitor (display);
    GdkRectangle area = { 0, 0, 1024, 768 };
    int width, height;

    if (!monitor)
      monitor = gdk_display_get_monitor (display, 0);
    if (monitor)
      gdk_monitor_get_workarea (monitor, &area);

    width = MIN (640, area.width);
    height = MAX (300, MIN (640, area.height * 8 / 10));
    gtk_window_set_default_size (GTK_WINDOW (p->dialog), width, height);
  }
  gtk_window_set_transient_for (GTK_WINDOW (p->dialog), GTK_WINDOW (p->fake_parent));
  gtk_window_set_modal (GTK_WINDOW (p->dialog), TRUE);

  cancel_button = gtk_dialog_add_button (GTK_DIALOG (p->dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  p->share_button = gtk_dialog_add_button (GTK_DIALOG (p->dialog), _("Share"), GTK_RESPONSE_ACCEPT);
  gtk_style_context_add_class (gtk_widget_get_style_context (p->share_button), "suggested-action");
  gtk_widget_set_sensitive (p->share_button, FALSE);
  gtk_dialog_set_default_response (GTK_DIALOG (p->dialog), GTK_RESPONSE_ACCEPT);

  content = gtk_dialog_get_content_area (GTK_DIALOG (p->dialog));
  gtk_container_set_border_width (GTK_CONTAINER (content), 12);
  gtk_box_set_spacing (GTK_BOX (content), 12);

  p->list = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (p->list),
                                   multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (p->list), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (p->list), 2);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (p->list), TRUE);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (p->list), 6);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (p->list), 6);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (p->list), FALSE);
  for (i = 0; i < sources->len; i++)
    {
      GtkWidget *card = card_new (g_ptr_array_index (sources, i));

      gtk_container_add (GTK_CONTAINER (p->list), card);
      g_ptr_array_add (p->cards, card);
    }

  p->search = gtk_search_entry_new ();
  gtk_widget_set_name (p->search, "screencast-search");
  gtk_entry_set_placeholder_text (GTK_ENTRY (p->search), _("Search windows and screens"));
  /* "changed", not the delayed "search-changed": filter on every keystroke */
  g_signal_connect (p->search, "changed", G_CALLBACK (on_search_changed), p);
  gtk_box_pack_start (GTK_BOX (content), p->search, FALSE, FALSE, 0);
  gtk_flow_box_set_filter_func (GTK_FLOW_BOX (p->list), filter_func, p, NULL);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), p->list);
  gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);

  g_signal_connect (p->list, "selected-children-changed", G_CALLBACK (on_selection_changed), p);
  g_signal_connect (p->list, "child-activated", G_CALLBACK (on_child_activated), p);
  g_signal_connect (p->dialog, "response", G_CALLBACK (on_response), p);

  if (p->cancellable)
    p->cancelled_id = g_cancellable_connect (p->cancellable, G_CALLBACK (on_cancelled), p, NULL);

  gtk_widget_show_all (content);
  gtk_widget_realize (p->dialog);

  /* A focused list box selects its first row: start with nothing chosen so
   * the user has to pick what they share. */
  gtk_widget_grab_focus (p->search);
  (void) cancel_button;
  gtk_flow_box_unselect_all (GTK_FLOW_BOX (p->list));

  if (p->external_parent)
    external_window_set_parent_of (p->external_parent, gtk_widget_get_window (p->dialog));

  gtk_window_present (GTK_WINDOW (p->dialog));
  p->loader_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, load_next_thumbnail, p, NULL);
}

GArray *
screencast_dialog_run_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
