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

#include <X11/Xlib.h>

#include "compositing.h"

gboolean
screencast_compositing_active (void)
{
  Display *dpy = XOpenDisplay (NULL);
  g_autofree char *name = NULL;
  gboolean active;

  if (!dpy)
    return FALSE;

  name = g_strdup_printf ("_NET_WM_CM_S%d", DefaultScreen (dpy));
  active = XGetSelectionOwner (dpy, XInternAtom (dpy, name, False)) != None;
  XCloseDisplay (dpy);

  return active;
}
