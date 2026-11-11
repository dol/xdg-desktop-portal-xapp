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

void
screencast_dialog_run_async (ExternalWindow *external_parent,
                             GPtrArray *sources,
                             gboolean multiple,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);

  g_task_set_task_data (task, g_ptr_array_ref (sources), (GDestroyNotify) g_ptr_array_unref);

  /* Test hook: never shown to users, only active when the harness sets it */
  if (g_getenv ("XAPP_SCREENCAST_TEST_AUTOPICK"))
    {
      g_idle_add (autopick_idle, task);
      return;
    }

  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "No picker available");
  g_object_unref (task);
}

GArray *
screencast_dialog_run_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
