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
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
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

  /* Windows we redirected so their contents exist while obscured */
  GHashTable *redirected;

  /* drawable -> Damage; drawable -> dirty flag (GINT_TO_POINTER (1)) */
  int damage_event_base;
  GHashTable *damages;
  GHashTable *dirty;
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

static char *
window_title (X11Capture *c, Window w)
{
  Atom utf8 = XInternAtom (c->dpy, "UTF8_STRING", False);
  Atom prop = XInternAtom (c->dpy, "_NET_WM_NAME", False);
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char *data = NULL;
  char *title = NULL;

  if (XGetWindowProperty (c->dpy, w, prop, 0, 1024, False, utf8, &type, &format,
                          &n, &after, &data) == Success && data)
    {
      if (type == utf8 && n > 0)
        title = g_strndup ((char *) data, n);
      XFree (data);
    }

  if (!title)
    {
      char *legacy = NULL;

      if (XFetchName (c->dpy, w, &legacy) && legacy)
        {
          title = g_strdup (legacy);
          XFree (legacy);
        }
    }

  return title ? title : g_strdup ("");
}

static gboolean
window_is_shareable (X11Capture *c, Window w)
{
  Atom type_prop = XInternAtom (c->dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom normal = XInternAtom (c->dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
  Atom dialog = XInternAtom (c->dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  XWindowAttributes attrs;
  Atom type;
  int format;
  unsigned long n, after, i;
  unsigned char *data = NULL;
  gboolean ok = TRUE;

  if (!XGetWindowAttributes (c->dpy, w, &attrs) || attrs.map_state != IsViewable)
    return FALSE;

  if (XGetWindowProperty (c->dpy, w, type_prop, 0, 16, False, XA_ATOM, &type, &format,
                          &n, &after, &data) == Success && data)
    {
      if (type == XA_ATOM && n > 0)
        {
          ok = FALSE;
          for (i = 0; i < n; i++)
            {
              Atom a = ((Atom *) data)[i];

              if (a == normal || a == dialog)
                ok = TRUE;
            }
        }
      XFree (data);
    }

  return ok;
}

static void
list_windows (X11Capture *c, GPtrArray *out)
{
  Atom list_prop = XInternAtom (c->dpy, "_NET_CLIENT_LIST", False);
  Atom type;
  int format;
  unsigned long n, after, i;
  unsigned char *data = NULL;

  if (XGetWindowProperty (c->dpy, c->root, list_prop, 0, 4096, False, XA_WINDOW,
                          &type, &format, &n, &after, &data) != Success || !data)
    return;

  for (i = 0; type == XA_WINDOW && i < n; i++)
    {
      Window w = ((Window *) data)[i];
      XWindowAttributes attrs;
      g_autofree char *title = NULL;

      trap_push (c->dpy);
      if (window_is_shareable (c, w) && XGetWindowAttributes (c->dpy, w, &attrs))
        title = window_title (c, w);
      if (trap_pop (c->dpy) != 0 || !title)
        continue;

      g_ptr_array_add (out, source_new (w, SCREENCAST_SOURCE_WINDOW, title,
                                        attrs.x, attrs.y, attrs.width, attrs.height));
    }

  XFree (data);
}

/* Draws the pointer image over dst, whose (0,0) is root coordinate (ox,oy) */
static void
embed_cursor_image (X11Capture *c, int ox, int oy, int width, int height,
                    guint8 *dst, gsize stride)
{
  XFixesCursorImage *img = XFixesGetCursorImage (c->dpy);
  int cx, cy, x, y;

  if (!img)
    return;

  cx = img->x - img->xhot - ox;
  cy = img->y - img->yhot - oy;

  for (y = 0; y < img->height; y++)
    {
      for (x = 0; x < img->width; x++)
        {
          unsigned long argb = img->pixels[y * img->width + x];
          guint8 a = (argb >> 24) & 0xff;
          guint8 *px;
          int dx = cx + x, dy = cy + y;

          if (a == 0 || dx < 0 || dy < 0 || dx >= width || dy >= height)
            continue;

          /* Cursor pixels are premultiplied: out = src + dst * (1 - a) */
          px = dst + dy * stride + dx * 4;
          px[0] = (argb & 0xff) + px[0] * (255 - a) / 255;
          px[1] = ((argb >> 8) & 0xff) + px[1] * (255 - a) / 255;
          px[2] = ((argb >> 16) & 0xff) + px[2] * (255 - a) / 255;
        }
    }

  XFree (img);
}

static gboolean
grab_window (X11Capture *c, const ScreenCastSource *s, guint8 *dst, gsize stride,
             int *origin_x, int *origin_y, GError **error)
{
  Window w = s->id;
  Window child;
  Pixmap pixmap;
  Window root_ret;
  int gx, gy, rx, ry;
  unsigned int pw, ph, border, depth;
  g_autofree guint8 *tmp = NULL;
  gsize tmp_stride;
  int row, code;

  if (!g_hash_table_contains (c->redirected, GUINT_TO_POINTER (w)))
    {
      trap_push (c->dpy);
      XCompositeRedirectWindow (c->dpy, w, CompositeRedirectAutomatic);
      code = trap_pop (c->dpy);
      if (code != 0)
        goto gone;

      g_hash_table_add (c->redirected, GUINT_TO_POINTER (w));
    }

  trap_push (c->dpy);
  pixmap = XCompositeNameWindowPixmap (c->dpy, w);
  if (XGetGeometry (c->dpy, pixmap, &root_ret, &gx, &gy, &pw, &ph, &border, &depth))
    XTranslateCoordinates (c->dpy, w, c->root, 0, 0, &rx, &ry, &child);
  code = trap_pop (c->dpy);

  if (code != 0 || pw == 0 || ph == 0)
    {
      /* On an X error the pixmap id was never created */
      if (code == 0)
        XFreePixmap (c->dpy, pixmap);
      goto gone;
    }

  /* The window may have been resized since the stream was sized: copy the
   * overlap and leave the rest black. */
  tmp_stride = pw * 4;
  tmp = g_malloc (tmp_stride * ph);
  if (!grab_drawable (c, pixmap, FALSE, 0, 0, pw, ph, tmp, tmp_stride, error))
    {
      XFreePixmap (c->dpy, pixmap);
      return FALSE;
    }
  XFreePixmap (c->dpy, pixmap);

  for (row = 0; row < s->height; row++)
    {
      memset (dst + row * stride, 0, s->width * 4);
      if (row < (int) ph)
        memcpy (dst + row * stride, tmp + row * tmp_stride, MIN (s->width, (int) pw) * 4);
    }

  *origin_x = rx;
  *origin_y = ry;
  return TRUE;

gone:
  g_hash_table_remove (c->redirected, GUINT_TO_POINTER (w));
  g_set_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_SOURCE_GONE,
               "Window 0x%lx is gone or cannot be captured", (unsigned long) w);
  return FALSE;
}

static GPtrArray *
x11_list_sources (ScreenCastCapture *base, guint types)
{
  X11Capture *c = (X11Capture *) base;
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) screencast_source_free);

  if (types & SCREENCAST_SOURCE_MONITOR)
    list_monitors (c, out);

  if (types & SCREENCAST_SOURCE_WINDOW)
    list_windows (c, out);

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
  int ox, oy;

  if (s->type == SCREENCAST_SOURCE_MONITOR)
    {
      ox = s->x;
      oy = s->y;

      if (!grab_drawable (c, c->root, TRUE, s->x, s->y, s->width, s->height, dst, stride, error))
        return FALSE;
    }
  else if (s->type == SCREENCAST_SOURCE_WINDOW)
    {
      if (!grab_window (c, s, dst, stride, &ox, &oy, error))
        return FALSE;
    }
  else
    {
      g_set_error (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_FAILED,
                   "Unsupported source type %d", s->type);
      return FALSE;
    }

  if (embed_cursor)
    embed_cursor_image (c, ox, oy, s->width, s->height, dst, stride);

  return TRUE;
}

static Drawable
source_drawable (X11Capture *c, const ScreenCastSource *s)
{
  return s->type == SCREENCAST_SOURCE_WINDOW ? (Drawable) s->id : (Drawable) c->root;
}

static gboolean
x11_refresh (ScreenCastCapture *base, ScreenCastSource *s)
{
  X11Capture *c = (X11Capture *) base;

  if (s->type == SCREENCAST_SOURCE_MONITOR)
    {
      g_autoptr(GPtrArray) monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) screencast_source_free);
      guint i;

      list_monitors (c, monitors);
      for (i = 0; i < monitors->len; i++)
        {
          ScreenCastSource *m = g_ptr_array_index (monitors, i);

          if (m->id == s->id)
            {
              s->x = m->x;
              s->y = m->y;
              s->width = m->width;
              s->height = m->height;
              return TRUE;
            }
        }

      return FALSE;
    }
  else
    {
      XWindowAttributes attrs;
      gboolean ok;

      trap_push (c->dpy);
      ok = XGetWindowAttributes (c->dpy, s->id, &attrs);
      ok = trap_pop (c->dpy) == 0 && ok && attrs.map_state == IsViewable;

      if (ok)
        {
          s->width = attrs.width;
          s->height = attrs.height;
        }

      return ok;
    }
}

static gboolean
x11_poll_damage (ScreenCastCapture *base, const ScreenCastSource *s)
{
  X11Capture *c = (X11Capture *) base;
  Drawable d = source_drawable (c, s);
  gpointer key = GUINT_TO_POINTER (d);
  GHashTableIter iter;
  gpointer damage;

  if (!g_hash_table_contains (c->damages, key))
    {
      Damage dmg;

      trap_push (c->dpy);
      dmg = XDamageCreate (c->dpy, d, XDamageReportNonEmpty);
      if (trap_pop (c->dpy) == 0)
        g_hash_table_insert (c->damages, key, GUINT_TO_POINTER (dmg));

      return TRUE;
    }

  while (XPending (c->dpy))
    {
      XEvent ev;

      XNextEvent (c->dpy, &ev);
      if (ev.type == c->damage_event_base + XDamageNotify)
        g_hash_table_insert (c->dirty, GUINT_TO_POINTER (((XDamageNotifyEvent *) &ev)->drawable),
                             GINT_TO_POINTER (1));
    }

  /* Re-arm: NonEmpty reports only fire again after the region is subtracted */
  trap_push (c->dpy);
  g_hash_table_iter_init (&iter, c->damages);
  while (g_hash_table_iter_next (&iter, NULL, &damage))
    XDamageSubtract (c->dpy, (Damage) GPOINTER_TO_UINT (damage), None, None);
  trap_pop (c->dpy);

  return g_hash_table_remove (c->dirty, key);
}

static void
x11_free (ScreenCastCapture *base)
{
  X11Capture *c = (X11Capture *) base;

  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, c->redirected);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      trap_push (c->dpy);
      XCompositeUnredirectWindow (c->dpy, (Window) GPOINTER_TO_UINT (key), CompositeRedirectAutomatic);
      trap_pop (c->dpy);
    }
  g_hash_table_destroy (c->redirected);

  g_hash_table_iter_init (&iter, c->damages);
  while (g_hash_table_iter_next (&iter, NULL, &key))
    {
      trap_push (c->dpy);
      XDamageDestroy (c->dpy, (Damage) GPOINTER_TO_UINT (key));
      trap_pop (c->dpy);
    }
  g_hash_table_destroy (c->damages);
  g_hash_table_destroy (c->dirty);

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
  c->base.refresh = x11_refresh;
  c->base.poll_damage = x11_poll_damage;
  c->base.free = x11_free;
  c->dpy = dpy;
  c->screen = DefaultScreen (dpy);
  c->root = RootWindow (dpy, c->screen);
  c->have_shm = XShmQueryExtension (dpy);
  c->redirected = g_hash_table_new (g_direct_hash, g_direct_equal);
  c->damages = g_hash_table_new (g_direct_hash, g_direct_equal);
  c->dirty = g_hash_table_new (g_direct_hash, g_direct_equal);
  XDamageQueryExtension (dpy, &c->damage_event_base, &(int){ 0 });

  return &c->base;
}
