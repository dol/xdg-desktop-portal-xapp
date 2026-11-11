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

#include <glib.h>

#include "capture.h"

typedef struct _ScreenCastStream ScreenCastStream;

/* The stream uses `c` from the main thread only and expects it to be used
 * by no other stream (damage tracking is per capture). It copies `s`. */
ScreenCastStream *screencast_stream_new (ScreenCastCapture *c,
                                         const ScreenCastSource *s,
                                         gboolean embed_cursor,
                                         GError **error);

/* PipeWire node id, valid as soon as screencast_stream_new() returns */
guint32 screencast_stream_get_node_id (ScreenCastStream *stream);

/* Called once, from the main loop, when the source disappears or the
 * PipeWire connection fails. The callback may destroy the stream later,
 * but not from inside the callback. */
typedef void (*ScreenCastStreamClosedFunc) (ScreenCastStream *stream,
                                            gpointer user_data);

void screencast_stream_set_closed_callback (ScreenCastStream *stream,
                                            ScreenCastStreamClosedFunc cb,
                                            gpointer user_data);

void screencast_stream_destroy (ScreenCastStream *stream);
