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

/* TRUE while a compositing manager owns _NET_WM_CM_S<screen> */
gboolean screencast_compositing_active (void);

typedef void (*ScreenCastCompositingFunc) (gboolean active,
                                           gpointer user_data);

/* Calls `func` from the main loop whenever a compositing manager appears or
 * goes away. Only one watch exists at a time. */
void screencast_compositing_watch (ScreenCastCompositingFunc func,
                                   gpointer user_data);
void screencast_compositing_unwatch (void);
