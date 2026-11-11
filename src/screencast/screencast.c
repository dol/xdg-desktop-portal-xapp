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

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"

#include "screencast.h"
#include "screencastdialog.h"
#include "capture.h"
#include "compositing.h"
#include "stream.h"

#include "externalwindow.h"
#include "request.h"
#include "session.h"
#include "utils.h"

#define CURSOR_MODE_HIDDEN 1
#define CURSOR_MODE_EMBEDDED 2
#define AVAILABLE_CURSOR_MODES (CURSOR_MODE_HIDDEN | CURSOR_MODE_EMBEDDED)
#define SCREENCAST_VERSION 4

static XdpImplScreenCast *impl;

typedef struct
{
  ScreenCastCapture *capture;
  ScreenCastStream *stream;
  ScreenCastSource source;
} ActiveStream;

typedef struct _ScreenCastSession ScreenCastSession;
typedef struct _ScreenCastSessionClass ScreenCastSessionClass;

struct _ScreenCastSession
{
  Session parent;

  guint selected_types;
  guint cursor_mode;
  gboolean multiple;
  GList *streams;
  guint name_watch;
};

struct _ScreenCastSessionClass
{
  SessionClass parent_class;
};

GType screencast_session_get_type (void);

G_DEFINE_TYPE (ScreenCastSession, screencast_session, session_get_type ())

static guint
available_source_types (void)
{
  return SCREENCAST_SOURCE_MONITOR |
         (screencast_compositing_active () ? SCREENCAST_SOURCE_WINDOW : 0);
}

static void
active_stream_free (ActiveStream *as)
{
  screencast_stream_destroy (as->stream);
  screencast_capture_free (as->capture);
  g_free (as->source.name);
  g_free (as);
}

static void
screencast_session_close (Session *session)
{
  ScreenCastSession *self = (ScreenCastSession *) session;

  g_debug ("Closing screencast session %s", session->id);

  if (self->name_watch)
    g_bus_unwatch_name (self->name_watch);
  self->name_watch = 0;

  g_list_free_full (self->streams, (GDestroyNotify) active_stream_free);
  self->streams = NULL;
}

static void
screencast_session_init (ScreenCastSession *self)
{
  self->selected_types = SCREENCAST_SOURCE_MONITOR;
  self->cursor_mode = CURSOR_MODE_HIDDEN;
}

static void
screencast_session_class_init (ScreenCastSessionClass *klass)
{
  ((SessionClass *) klass)->close = screencast_session_close;
}

static void
close_and_notify (ScreenCastSession *self)
{
  Session *session = (Session *) self;

  if (session->closed)
    return;

  /* Tell the frontend before the object goes away */
  xdp_impl_session_emit_closed (XDP_IMPL_SESSION (session));
  session_close (session);
}

static void
on_client_vanished (GDBusConnection *connection,
                    const char *name,
                    gpointer user_data)
{
  close_and_notify (user_data);
}

static gboolean
close_idle (gpointer data)
{
  close_and_notify (data);
  g_object_unref (data);

  return G_SOURCE_REMOVE;
}

static void
on_stream_closed (ScreenCastStream *stream, gpointer user_data)
{
  /* Not from inside the stream's own callback */
  g_idle_add (close_idle, g_object_ref (user_data));
}

static ScreenCastSession *
lookup_screencast_session (const char *handle)
{
  Session *session = lookup_session (handle);

  if (session && G_TYPE_CHECK_INSTANCE_TYPE (session, screencast_session_get_type ()) && !session->closed)
    return (ScreenCastSession *) session;

  return NULL;
}

/* --- CreateSession ---------------------------------------------------- */

static gboolean
handle_create_session (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  ScreenCastSession *self;
  GVariantBuilder results;

  g_debug ("CreateSession for %s: %s", arg_app_id, arg_session_handle);

  self = g_object_new (screencast_session_get_type (), "id", arg_session_handle, NULL);

  if (!session_export ((Session *) self, connection, &error))
    {
      g_warning ("Failed to export screencast session: %s", error->message);
      g_object_unref (self);
      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_impl_screen_cast_complete_create_session (object, invocation, 2,
                                                    g_variant_builder_end (&results));
      return TRUE;
    }

  self->name_watch = g_bus_watch_name_on_connection (connection,
                                                     g_dbus_method_invocation_get_sender (invocation),
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     NULL, on_client_vanished, self, NULL);

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screen_cast_complete_create_session (object, invocation, 0,
                                                g_variant_builder_end (&results));

  return TRUE;
}

/* --- SelectSources ---------------------------------------------------- */

static gboolean
handle_select_sources (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  ScreenCastSession *self = lookup_screencast_session (arg_session_handle);
  guint types = SCREENCAST_SOURCE_MONITOR;
  guint cursor_mode = CURSOR_MODE_HIDDEN;
  gboolean multiple = FALSE;
  GVariantBuilder results;
  int response = 0;

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);

  if (!self)
    {
      response = 2;
      goto out;
    }

  g_variant_lookup (arg_options, "types", "u", &types);
  g_variant_lookup (arg_options, "multiple", "b", &multiple);
  g_variant_lookup (arg_options, "cursor_mode", "u", &cursor_mode);

  /* A cursor mode we do not advertise ends the session, per the portal spec */
  if (cursor_mode == 0 || (cursor_mode & ~AVAILABLE_CURSOR_MODES) != 0 ||
      (cursor_mode & (cursor_mode - 1)) != 0)
    {
      g_warning ("Rejecting unsupported cursor_mode %u", cursor_mode);
      close_and_notify (self);
      response = 2;
      goto out;
    }

  self->selected_types = types & (SCREENCAST_SOURCE_MONITOR | SCREENCAST_SOURCE_WINDOW);
  self->cursor_mode = cursor_mode;
  self->multiple = multiple;

  if (self->selected_types == 0)
    response = 2;

out:
  xdp_impl_screen_cast_complete_select_sources (object, invocation, response,
                                                g_variant_builder_end (&results));

  return TRUE;
}

/* --- Start ------------------------------------------------------------ */

typedef struct
{
  XdpImplScreenCast *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  ScreenCastSession *session;
  GPtrArray *sources;
  ScreenCastCapture *capture;   /* renders the picker previews; lives as long as the dialog */
  GCancellable *cancellable;
  ExternalWindow *external_parent;
  gboolean responded;
} StartHandle;

static void
start_handle_free (StartHandle *h)
{
  if (h->request->exported)
    request_unexport (h->request);

  g_clear_object (&h->external_parent);
  g_object_unref (h->request);
  g_object_unref (h->session);
  g_object_unref (h->cancellable);
  g_ptr_array_unref (h->sources);
  screencast_capture_free (h->capture);
  g_free (h);
}

static void
start_respond (StartHandle *h, int response, GVariant *results)
{
  GVariantBuilder empty;

  if (!results)
    {
      g_variant_builder_init (&empty, G_VARIANT_TYPE_VARDICT);
      results = g_variant_builder_end (&empty);
    }

  h->responded = TRUE;
  xdp_impl_screen_cast_complete_start (h->impl, h->invocation, response, results);
}

static const ScreenCastSource *
find_source (GPtrArray *sources, guint32 id)
{
  guint i;

  for (i = 0; i < sources->len; i++)
    {
      ScreenCastSource *s = g_ptr_array_index (sources, i);

      if (s->id == id)
        return s;
    }

  return NULL;
}

static gboolean
start_streams (StartHandle *h, GArray *ids)
{
  ScreenCastSession *self = h->session;
  guint i;

  for (i = 0; i < ids->len && (self->multiple || i == 0); i++)
    {
      const ScreenCastSource *picked = find_source (h->sources, g_array_index (ids, guint32, i));
      g_autoptr(GError) error = NULL;
      ActiveStream *as;

      if (!picked)
        return FALSE;

      /* The picker listed windows while a compositor ran; it may be gone now */
      if (picked->type == SCREENCAST_SOURCE_WINDOW && !screencast_compositing_active ())
        {
          g_warning ("Refusing window capture: compositing was turned off");
          return FALSE;
        }

      as = g_new0 (ActiveStream, 1);
      as->source = *picked;
      as->source.name = g_strdup (picked->name);

      as->capture = screencast_capture_x11_new (&error);
      if (as->capture)
        as->stream = screencast_stream_new (as->capture, &as->source,
                                            self->cursor_mode == CURSOR_MODE_EMBEDDED, &error);
      if (!as->stream)
        {
          g_warning ("Could not start screen cast stream: %s", error ? error->message : "unknown error");
          screencast_capture_free (as->capture);
          g_free (as->source.name);
          g_free (as);
          return FALSE;
        }

      screencast_stream_set_closed_callback (as->stream, on_stream_closed, self);
      self->streams = g_list_append (self->streams, as);
    }

  return self->streams != NULL;
}

static GVariant *
streams_result (ScreenCastSession *self)
{
  GVariantBuilder streams, results;
  GList *l;

  g_variant_builder_init (&streams, G_VARIANT_TYPE ("a(ua{sv})"));

  for (l = self->streams; l; l = l->next)
    {
      ActiveStream *as = l->data;
      GVariantBuilder props;

      g_variant_builder_init (&props, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&props, "{sv}", "id", g_variant_new_take_string (g_strdup_printf ("%u", as->source.id)));
      g_variant_builder_add (&props, "{sv}", "source_type", g_variant_new_uint32 (as->source.type));
      g_variant_builder_add (&props, "{sv}", "position", g_variant_new ("(ii)", as->source.x, as->source.y));
      g_variant_builder_add (&props, "{sv}", "size", g_variant_new ("(ii)", as->source.width, as->source.height));
      g_variant_builder_add (&streams, "(ua{sv})", screencast_stream_get_node_id (as->stream), &props);
    }

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&results, "{sv}", "streams", g_variant_builder_end (&streams));

  return g_variant_builder_end (&results);
}

static void
on_dialog_done (GObject *source, GAsyncResult *result, gpointer data)
{
  StartHandle *h = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GArray) ids = screencast_dialog_run_finish (result, &error);

  /* Request.Close already answered it */
  if (h->responded)
    {
      start_handle_free (h);
      return;
    }

  if (!ids)
    {
      start_respond (h, g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? 1 : 2, NULL);
    }
  else if (h->session->parent.closed || !start_streams (h, ids))
    {
      g_list_free_full (h->session->streams, (GDestroyNotify) active_stream_free);
      h->session->streams = NULL;
      start_respond (h, 2, NULL);
    }
  else
    {
      start_respond (h, 0, streams_result (h->session));
    }

  start_handle_free (h);
}

static gboolean
handle_start_close (XdpImplRequest *object,
                    GDBusMethodInvocation *invocation,
                    StartHandle *h)
{
  if (!h->responded)
    {
      start_respond (h, 2, NULL);
      g_cancellable_cancel (h->cancellable);
    }

  xdp_impl_request_complete_close (object, invocation);

  return TRUE;
}

static gboolean
handle_start (XdpImplScreenCast *object,
              GDBusMethodInvocation *invocation,
              const char *arg_handle,
              const char *arg_session_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  ScreenCastSession *session = lookup_screencast_session (arg_session_handle);
  g_autoptr(GError) error = NULL;
  ScreenCastCapture *capture = NULL;
  GPtrArray *sources = NULL;
  StartHandle *h;
  GVariantBuilder empty;
  guint types;

  if (session)
    {
      capture = screencast_capture_x11_new (&error);

      /* What can be offered right now, not what was selected earlier */
      types = session->selected_types & available_source_types ();
      if (capture && types)
        sources = screencast_capture_list_sources (capture, types);
    }

  if (!sources || sources->len == 0)
    {
      if (sources)
        g_ptr_array_unref (sources);
      screencast_capture_free (capture);
      g_variant_builder_init (&empty, G_VARIANT_TYPE_VARDICT);
      xdp_impl_screen_cast_complete_start (object, invocation, 2, g_variant_builder_end (&empty));
      return TRUE;
    }

  h = g_new0 (StartHandle, 1);
  h->impl = object;
  h->invocation = invocation;
  h->session = g_object_ref (session);
  h->sources = sources;
  h->capture = capture;
  h->cancellable = g_cancellable_new ();
  h->request = request_new (g_dbus_method_invocation_get_sender (invocation), arg_app_id, arg_handle);

  if (arg_parent_window && *arg_parent_window)
    {
      h->external_parent = create_external_window_from_handle (arg_parent_window);
      if (!h->external_parent)
        g_warning ("Failed to associate portal window with parent window %s", arg_parent_window);
    }

  g_signal_connect (h->request, "handle-close", G_CALLBACK (handle_start_close), h);
  request_export (h->request, g_dbus_method_invocation_get_connection (invocation));

  screencast_dialog_run_async (h->external_parent, h->sources, h->capture, session->multiple,
                               h->cancellable, on_dialog_done, h);

  return TRUE;
}

/* --- Init --------------------------------------------------------------- */

static void
on_compositing_changed (gboolean active, gpointer user_data)
{
  xdp_impl_screen_cast_set_available_source_types (impl,
                                                   SCREENCAST_SOURCE_MONITOR |
                                                   (active ? SCREENCAST_SOURCE_WINDOW : 0));
}

gboolean
screencast_init (GDBusConnection *bus, GError **error)
{
  GDBusInterfaceSkeleton *helper;

  impl = xdp_impl_screen_cast_skeleton_new ();
  helper = G_DBUS_INTERFACE_SKELETON (impl);

  xdp_impl_screen_cast_set_version (impl, SCREENCAST_VERSION);
  xdp_impl_screen_cast_set_available_cursor_modes (impl, AVAILABLE_CURSOR_MODES);
  xdp_impl_screen_cast_set_available_source_types (impl, available_source_types ());

  /* The frontend only learns about compositor changes through the property */
  screencast_compositing_watch (on_compositing_changed, NULL);

  g_signal_connect (helper, "handle-create-session", G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (helper, "handle-select-sources", G_CALLBACK (handle_select_sources), NULL);
  g_signal_connect (helper, "handle-start", G_CALLBACK (handle_start), NULL);

  if (!g_dbus_interface_skeleton_export (helper, bus, DESKTOP_PORTAL_OBJECT_PATH, error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
