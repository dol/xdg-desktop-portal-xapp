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

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "screencastdialog.h"

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

typedef struct
{
  GtkWidget *dialog;
  GtkWidget *list;
  GtkWidget *share_button;
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
  g_object_unref (p->task);
  g_free (p);
}

static void
on_response (GtkDialog *dialog, int response, Picker *p)
{
  if (response == GTK_RESPONSE_ACCEPT)
    {
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
      GList *rows = gtk_list_box_get_selected_rows (GTK_LIST_BOX (p->list));
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

static void
on_selection_changed (GtkListBox *list, Picker *p)
{
  GList *rows = gtk_list_box_get_selected_rows (list);

  gtk_widget_set_sensitive (p->share_button, rows != NULL);
  g_list_free (rows);
}

static void
on_row_activated (GtkListBox *list, GtkListBoxRow *row, Picker *p)
{
  /* Double-click shares straight away when only one thing can be picked */
  if (gtk_list_box_get_selection_mode (list) == GTK_SELECTION_SINGLE)
    gtk_dialog_response (GTK_DIALOG (p->dialog), GTK_RESPONSE_ACCEPT);
}

static void
header_func (GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
  guint type = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "source-type"));
  guint before_type = before ? GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (before), "source-type")) : 0;
  GtkWidget *label;

  if (before && before_type == type)
    {
      gtk_list_box_row_set_header (row, NULL);
      return;
    }

  label = gtk_label_new (type == SCREENCAST_SOURCE_MONITOR ? _("Monitors") : _("Windows"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_top (label, 6);
  gtk_widget_set_margin_start (label, 6);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
  gtk_widget_show (label);
  gtk_list_box_row_set_header (row, label);
}

static GtkWidget *
row_new (const ScreenCastSource *source)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *icon = gtk_image_new_from_icon_name (source->type == SCREENCAST_SOURCE_MONITOR ?
                                                  "video-display-symbolic" : "window-symbolic",
                                                  GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *name = gtk_label_new (source->name && *source->name ? source->name : _("Untitled"));
  g_autofree char *size_text = g_strdup_printf ("%d × %d", source->width, source->height);
  GtkWidget *size = gtk_label_new (size_text);

  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_xalign (GTK_LABEL (size), 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (size), "dim-label");

  gtk_box_pack_start (GTK_BOX (text), name, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (text), size, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), text, TRUE, TRUE, 0);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_widget_show_all (row);

  g_object_set_data (G_OBJECT (row), "source-id", GUINT_TO_POINTER (source->id));
  g_object_set_data (G_OBJECT (row), "source-type", GUINT_TO_POINTER (source->type));

  return row;
}

void
screencast_dialog_run_async (ExternalWindow *external_parent,
                             GPtrArray *sources,
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

  /* Test hook: never shown to users, only active when the harness sets it */
  if (g_getenv ("XAPP_SCREENCAST_TEST_AUTOPICK"))
    {
      g_idle_add (autopick_idle, task);
      return;
    }

  p = g_new0 (Picker, 1);
  p->task = task;
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
  gtk_window_set_default_size (GTK_WINDOW (p->dialog), 420, 400);
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

  p->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (p->list),
                                   multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
  gtk_list_box_set_header_func (GTK_LIST_BOX (p->list), header_func, NULL, NULL);
  for (i = 0; i < sources->len; i++)
    gtk_container_add (GTK_CONTAINER (p->list), row_new (g_ptr_array_index (sources, i)));

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), p->list);
  gtk_box_pack_start (GTK_BOX (content), scrolled, TRUE, TRUE, 0);

  g_signal_connect (p->list, "selected-rows-changed", G_CALLBACK (on_selection_changed), p);
  g_signal_connect (p->list, "row-activated", G_CALLBACK (on_row_activated), p);
  g_signal_connect (p->dialog, "response", G_CALLBACK (on_response), p);

  if (p->cancellable)
    p->cancelled_id = g_cancellable_connect (p->cancellable, G_CALLBACK (on_cancelled), p, NULL);

  gtk_widget_show_all (content);
  gtk_widget_realize (p->dialog);

  /* A focused list box selects its first row: start with nothing chosen so
   * the user has to pick what they share. */
  gtk_widget_grab_focus (cancel_button);
  gtk_list_box_unselect_all (GTK_LIST_BOX (p->list));

  if (p->external_parent)
    external_window_set_parent_of (p->external_parent, gtk_widget_get_window (p->dialog));

  gtk_window_present (GTK_WINDOW (p->dialog));
}

GArray *
screencast_dialog_run_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
