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

#include <glib-unix.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

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

static Display *watch_dpy;
static int xfixes_event_base;
static guint watch_source;
static ScreenCastCompositingFunc watch_func;
static gpointer watch_data;

static gboolean
on_watch_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  while (XPending (watch_dpy))
    {
      XEvent ev;

      XNextEvent (watch_dpy, &ev);
      if (ev.type == xfixes_event_base + XFixesSelectionNotify)
        {
          XFixesSelectionNotifyEvent *sel = (XFixesSelectionNotifyEvent *) &ev;

          watch_func (sel->owner != None, watch_data);
        }
    }

  return G_SOURCE_CONTINUE;
}

void
screencast_compositing_watch (ScreenCastCompositingFunc func, gpointer user_data)
{
  g_autofree char *name = NULL;
  int error_base;

  screencast_compositing_unwatch ();

  watch_dpy = XOpenDisplay (NULL);
  if (!watch_dpy || !XFixesQueryExtension (watch_dpy, &xfixes_event_base, &error_base))
    {
      if (watch_dpy)
        XCloseDisplay (watch_dpy);
      watch_dpy = NULL;
      return;
    }

  watch_func = func;
  watch_data = user_data;

  name = g_strdup_printf ("_NET_WM_CM_S%d", DefaultScreen (watch_dpy));
  XFixesSelectSelectionInput (watch_dpy, DefaultRootWindow (watch_dpy),
                              XInternAtom (watch_dpy, name, False),
                              XFixesSetSelectionOwnerNotifyMask);
  /* Wait until the server has the registration: with only a flush, a change
   * made by another client right after this call can be missed. */
  XSync (watch_dpy, False);

  watch_source = g_unix_fd_add (ConnectionNumber (watch_dpy), G_IO_IN, on_watch_readable, NULL);
}

void
screencast_compositing_unwatch (void)
{
  if (watch_source)
    g_source_remove (watch_source);
  watch_source = 0;

  if (watch_dpy)
    XCloseDisplay (watch_dpy);
  watch_dpy = NULL;
}
