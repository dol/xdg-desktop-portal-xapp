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

#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

#include "capture.h"

typedef struct
{
  ScreenCastCapture base;

  Display *dpy;
  int screen;
  Window root;

  gboolean have_shm;
  XImage *image;
  XShmSegmentInfo shm;
  int image_width;
  int image_height;
} X11Capture;

G_DEFINE_QUARK (screencast-capture-error-quark, screencast_capture_error)

void
screencast_source_free (ScreenCastSource *source)
{
  if (!source)
    return;

  g_free (source->name);
  g_free (source);
}

/* Errors raised by requests that target windows or pixmaps owned by other
 * clients (BadWindow, BadMatch, BadDrawable) must not abort the portal. */
static int trapped_error_code = 0;
static XErrorHandler old_error_handler = NULL;

static int
trap_handler (Display *dpy, XErrorEvent *event)
{
  trapped_error_code = event->error_code;
  return 0;
}

static void
trap_push (Display *dpy)
{
  XSync (dpy, False);
  trapped_error_code = 0;
  old_error_handler = XSetErrorHandler (trap_handler);
}

static int
trap_pop (Display *dpy)
{
  XSync (dpy, False);
  XSetErrorHandler (old_error_handler);
  return trapped_error_code;
}

static void
destroy_image (X11Capture *c)
{
  if (!c->image)
    return;

  if (c->shm.shmaddr)
    {
      XShmDetach (c->dpy, &c->shm);
      XSync (c->dpy, False);
    }

  c->image->data = NULL;
  XDestroyImage (c->image);
  c->image = NULL;

  if (c->shm.shmaddr)
    {
      shmdt (c->shm.shmaddr);
      shmctl (c->shm.shmid, IPC_RMID, NULL);
    }

  memset (&c->shm, 0, sizeof (c->shm));
}

static gboolean
ensure_shm_image (X11Capture *c, int width, int height)
{
  if (c->image && c->image_width == width && c->image_height == height)
    return TRUE;

  destroy_image (c);

  c->image = XShmCreateImage (c->dpy, DefaultVisual (c->dpy, c->screen),
                              DefaultDepth (c->dpy, c->screen), ZPixmap,
                              NULL, &c->shm, width, height);
  if (!c->image)
    return FALSE;

  c->shm.shmid = shmget (IPC_PRIVATE, c->image->bytes_per_line * c->image->height,
                         IPC_CREAT | 0600);
  if (c->shm.shmid < 0)
    goto fail;

  c->shm.shmaddr = c->image->data = shmat (c->shm.shmid, NULL, 0);
  if (c->shm.shmaddr == (char *) -1)
    {
      shmctl (c->shm.shmid, IPC_RMID, NULL);
      c->shm.shmaddr = NULL;
      goto fail;
    }

  c->shm.readOnly = False;
  trap_push (c->dpy);
  XShmAttach (c->dpy, &c->shm);
  if (trap_pop (c->dpy) != 0)
    {
      shmdt (c->shm.shmaddr);
      shmctl (c->shm.shmid, IPC_RMID, NULL);
      c->shm.shmaddr = NULL;
      goto fail;
    }

  c->image_width = width;
  c->image_height = height;
  return TRUE;

fail:
  if (c->image)
    {
      c->image->data = NULL;
      XDestroyImage (c->image);
      c->image = NULL;
    }
  memset (&c->shm, 0, sizeof (c->shm));
  return FALSE;
}

static gboolean
copy_rows (XImage *image, int width, int height, guint8 *dst, gsize stride, GError **error)
{
  int row;

  if (image->bits_per_pixel != 32)
    {
      g_set_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_FAILED,
                   "Unsupported X11 pixel depth: %d bpp", image->bits_per_pixel);
      return FALSE;
    }

  for (row = 0; row < height; row++)
    memcpy (dst + row * stride, image->data + row * image->bytes_per_line, width * 4);

  return TRUE;
}

/* Reads a rectangle of `d` into dst. SHM is only used for drawables with
 * the default depth, since the shared image is created with it. */
static gboolean
grab_drawable (X11Capture *c,
               Drawable d,
               gboolean allow_shm,
               int x, int y, int width, int height,
               guint8 *dst, gsize stride,
               GError **error)
{
  XImage *image;
  gboolean ok;
  int code;

  if (allow_shm && c->have_shm && ensure_shm_image (c, width, height))
    {
      trap_push (c->dpy);
      ok = XShmGetImage (c->dpy, d, c->image, x, y, AllPlanes);
      code = trap_pop (c->dpy);

      if (ok && code == 0)
        return copy_rows (c->image, width, height, dst, stride, error);
    }

  trap_push (c->dpy);
  image = XGetImage (c->dpy, d, x, y, width, height, AllPlanes, ZPixmap);
  code = trap_pop (c->dpy);

  if (!image || code != 0)
    {
      if (image)
        XDestroyImage (image);

      g_set_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_SOURCE_GONE,
                   "Could not read X11 drawable (X error %d)", code);
      return FALSE;
    }

  ok = copy_rows (image, width, height, dst, stride, error);
  XDestroyImage (image);

  return ok;
}

static ScreenCastSource *
source_new (guint32 id, ScreenCastSourceType type, const char *name,
            int x, int y, int width, int height)
{
  ScreenCastSource *s = g_new0 (ScreenCastSource, 1);

  s->id = id;
  s->type = type;
  s->name = g_strdup (name);
  s->x = x;
  s->y = y;
  s->width = width;
  s->height = height;

  return s;
}

static void
list_monitors (X11Capture *c, GPtrArray *out)
{
  XRRScreenResources *res = XRRGetScreenResourcesCurrent (c->dpy, c->root);
  guint before = out->len;
  int i;

  for (i = 0; res && i < res->ncrtc; i++)
    {
      XRRCrtcInfo *crtc = XRRGetCrtcInfo (c->dpy, res, res->crtcs[i]);
      g_autofree char *name = NULL;

      if (crtc && crtc->mode != None && crtc->noutput > 0)
        {
          XRROutputInfo *output = XRRGetOutputInfo (c->dpy, res, crtc->outputs[0]);

          name = g_strdup (output ? output->name : "Monitor");
          if (output)
            XRRFreeOutputInfo (output);

          g_ptr_array_add (out, source_new (res->crtcs[i], SCREENCAST_SOURCE_MONITOR, name,
                                            crtc->x, crtc->y, crtc->width, crtc->height));
        }

      if (crtc)
        XRRFreeCrtcInfo (crtc);
    }

  if (res)
    XRRFreeScreenResources (res);

  /* No RandR crtc (some virtual servers): the whole screen is one monitor */
  if (out->len == before)
    g_ptr_array_add (out, source_new (0, SCREENCAST_SOURCE_MONITOR, "Screen", 0, 0,
                                      DisplayWidth (c->dpy, c->screen),
                                      DisplayHeight (c->dpy, c->screen)));
}

static GPtrArray *
x11_list_sources (ScreenCastCapture *base, guint types)
{
  X11Capture *c = (X11Capture *) base;
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) screencast_source_free);

  if (types & SCREENCAST_SOURCE_MONITOR)
    list_monitors (c, out);

  return out;
}

static gboolean
x11_grab (ScreenCastCapture *base,
          const ScreenCastSource *s,
          gboolean embed_cursor,
          guint8 *dst,
          gsize stride,
          GError **error)
{
  X11Capture *c = (X11Capture *) base;

  if (s->type != SCREENCAST_SOURCE_MONITOR)
    {
      g_set_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_FAILED,
                   "Unsupported source type %d", s->type);
      return FALSE;
    }

  return grab_drawable (c, c->root, TRUE, s->x, s->y, s->width, s->height, dst, stride, error);
}

static void
x11_free (ScreenCastCapture *base)
{
  X11Capture *c = (X11Capture *) base;

  destroy_image (c);
  XCloseDisplay (c->dpy);
  g_free (c);
}

ScreenCastCapture *
screencast_capture_x11_new (GError **error)
{
  X11Capture *c;
  Display *dpy = XOpenDisplay (NULL);

  if (!dpy)
    {
      g_set_error_literal (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_FAILED,
                           "Could not open X11 display");
      return NULL;
    }

  c = g_new0 (X11Capture, 1);
  c->base.list_sources = x11_list_sources;
  c->base.grab = x11_grab;
  c->base.free = x11_free;
  c->dpy = dpy;
  c->screen = DefaultScreen (dpy);
  c->root = RootWindow (dpy, c->screen);
  c->have_shm = XShmQueryExtension (dpy);

  return &c->base;
}
