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

typedef enum
{
  SCREENCAST_SOURCE_MONITOR = 1,
  SCREENCAST_SOURCE_WINDOW = 2,
} ScreenCastSourceType;

typedef struct
{
  guint32 id;                   /* CRTC id for monitors, XID for windows */
  ScreenCastSourceType type;
  char *name;
  gint x, y, width, height;
} ScreenCastSource;

typedef struct _ScreenCastCapture ScreenCastCapture;

struct _ScreenCastCapture
{
  GPtrArray *(*list_sources) (ScreenCastCapture *c,
                              guint types);
  /* Writes BGRx pixels, width * 4 bytes of each row, rows `stride` apart */
  gboolean (*grab) (ScreenCastCapture *c,
                    const ScreenCastSource *s,
                    gboolean embed_cursor,
                    guint8 *dst,
                    gsize stride,
                    GError **error);
  void (*free) (ScreenCastCapture *c);
};

#define SCREENCAST_CAPTURE_ERROR (screencast_capture_error_quark ())
GQuark screencast_capture_error_quark (void);

typedef enum
{
  SCREENCAST_CAPTURE_ERROR_FAILED,
  SCREENCAST_CAPTURE_ERROR_SOURCE_GONE,
} ScreenCastCaptureError;

void screencast_source_free (ScreenCastSource *source);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ScreenCastSource, screencast_source_free)

ScreenCastCapture *screencast_capture_x11_new (GError **error);

static inline GPtrArray *
screencast_capture_list_sources (ScreenCastCapture *c, guint types)
{
  return c->list_sources (c, types);
}

static inline gboolean
screencast_capture_grab (ScreenCastCapture *c,
                         const ScreenCastSource *s,
                         gboolean embed_cursor,
                         guint8 *dst,
                         gsize stride,
                         GError **error)
{
  return c->grab (c, s, embed_cursor, dst, stride, error);
}

static inline void
screencast_capture_free (ScreenCastCapture *c)
{
  if (c)
    c->free (c);
}
