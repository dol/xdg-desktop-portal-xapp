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

#pragma once

#include <gio/gio.h>

#include "capture.h"
#include "externalwindow.h"

/* Asks the user which of `sources` to share. Finishes with a GArray of
 * guint32 ScreenCastSource ids, or G_IO_ERROR_CANCELLED. At most one id is
 * returned unless `multiple`. `cancellable` dismisses the dialog. */
void screencast_dialog_run_async (ExternalWindow *external_parent,
                                  GPtrArray *sources,
                                  gboolean multiple,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

GArray *screencast_dialog_run_finish (GAsyncResult *result,
                                      GError **error);
