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

#include <errno.h>
#include <string.h>

#include <gio/gio.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "stream.h"

#define MAX_FRAMERATE 30
#define TICK_MS (1000 / MAX_FRAMERATE)
#define KEEPALIVE_US G_USEC_PER_SEC
#define REFRESH_TICKS (MAX_FRAMERATE / 2)
#define CONNECT_TIMEOUT_US (5 * G_USEC_PER_SEC)

struct _ScreenCastStream
{
  ScreenCastCapture *capture;
  ScreenCastSource source;
  gboolean embed_cursor;

  struct pw_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook stream_listener;
  GSource *loop_source;
  guint tick_id;

  guint32 node_id;
  gboolean failed;
  gboolean closed;

  /* Size the consumer negotiated; frames are only produced when it matches
   * the source, so a resize never overruns a buffer. */
  int negotiated_width;
  int negotiated_height;

  guint ticks;
  gint64 last_frame_us;
  guint64 seq;

  ScreenCastStreamClosedFunc closed_cb;
  gpointer closed_data;
};

/* --- PipeWire loop as a GSource ------------------------------------- */

typedef struct
{
  GSource source;
  struct pw_loop *loop;
} PwLoopSource;

static gboolean
pw_source_prepare (GSource *source, gint *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pw_source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
  pw_loop_iterate (((PwLoopSource *) source)->loop, 0);
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs pw_source_funcs = {
  pw_source_prepare,
  NULL,
  pw_source_dispatch,
  NULL,
};

static GSource *
pw_loop_source_new (struct pw_loop *loop)
{
  GSource *source = g_source_new (&pw_source_funcs, sizeof (PwLoopSource));

  ((PwLoopSource *) source)->loop = loop;
  g_source_add_unix_fd (source, pw_loop_get_fd (loop), G_IO_IN | G_IO_ERR | G_IO_HUP);

  return source;
}

/* --- Params ----------------------------------------------------------- */

static void
update_format_param (ScreenCastStream *s)
{
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  const struct spa_pod *params[1];

  params[0] = spa_pod_builder_add_object (
      &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_BGRx),
      SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle (&SPA_RECTANGLE (s->source.width, s->source.height)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&SPA_FRACTION (0, 1)),
      SPA_FORMAT_VIDEO_maxFramerate,
      SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (MAX_FRAMERATE, 1),
                                     &SPA_FRACTION (1, 1),
                                     &SPA_FRACTION (MAX_FRAMERATE, 1)));

  pw_stream_update_params (s->stream, params, 1);
}

static void
on_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  ScreenCastStream *s = data;
  struct spa_video_info_raw info;
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  const struct spa_pod *params[2];
  int stride;

  if (id != SPA_PARAM_Format || param == NULL)
    return;

  memset (&info, 0, sizeof (info));
  if (spa_format_video_raw_parse (param, &info) < 0)
    return;

  s->negotiated_width = info.size.width;
  s->negotiated_height = info.size.height;
  stride = info.size.width * 4;

  params[0] = spa_pod_builder_add_object (
      &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (4, 2, 16),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int (1),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int (stride * info.size.height),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int (stride),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int (1 << SPA_DATA_MemFd));

  params[1] = spa_pod_builder_add_object (
      &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_header)));

  pw_stream_update_params (s->stream, params, 2);
}

static void
on_state_changed (void *data,
                  enum pw_stream_state old,
                  enum pw_stream_state state,
                  const char *error)
{
  ScreenCastStream *s = data;

  if (state == PW_STREAM_STATE_ERROR)
    {
      g_warning ("PipeWire stream error: %s", error ? error : "unknown");
      s->failed = TRUE;
    }

  if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
    s->node_id = pw_stream_get_node_id (s->stream);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state_changed,
  .param_changed = on_param_changed,
};

/* --- Frame production ------------------------------------------------- */

static void
notify_closed (ScreenCastStream *s)
{
  if (s->closed)
    return;

  s->closed = TRUE;
  if (s->tick_id)
    {
      g_source_remove (s->tick_id);
      s->tick_id = 0;
    }

  if (s->closed_cb)
    s->closed_cb (s, s->closed_data);
}

static void
produce_frame (ScreenCastStream *s)
{
  struct pw_buffer *pwb;
  struct spa_buffer *buf;
  struct spa_meta_header *header;
  struct spa_data *d;
  gsize stride = s->source.width * 4;
  g_autoptr(GError) error = NULL;

  pwb = pw_stream_dequeue_buffer (s->stream);
  if (!pwb)
    return;

  buf = pwb->buffer;
  d = &buf->datas[0];

  if (d->data == NULL || d->maxsize < stride * s->source.height)
    {
      pw_stream_queue_buffer (s->stream, pwb);
      return;
    }

  if (!screencast_capture_grab (s->capture, &s->source, s->embed_cursor, d->data, stride, &error))
    {
      pw_stream_queue_buffer (s->stream, pwb);
      if (g_error_matches (error, SCREENCAST_CAPTURE_ERROR, SCREENCAST_CAPTURE_ERROR_SOURCE_GONE))
        notify_closed (s);
      else
        g_warning ("Screen capture failed: %s", error->message);
      return;
    }

  d->chunk->offset = 0;
  d->chunk->size = stride * s->source.height;
  d->chunk->stride = stride;

  header = spa_buffer_find_meta_data (buf, SPA_META_Header, sizeof (*header));
  if (header)
    {
      header->pts = g_get_monotonic_time () * 1000;
      header->flags = 0;
      header->seq = s->seq++;
      header->dts_offset = 0;
    }

  pw_stream_queue_buffer (s->stream, pwb);
  pw_stream_trigger_process (s->stream);
  s->last_frame_us = g_get_monotonic_time ();
}

static gboolean
on_tick (gpointer data)
{
  ScreenCastStream *s = data;
  gboolean dirty;

  if (s->failed)
    {
      notify_closed (s);
      return G_SOURCE_REMOVE;
    }

  if (s->ticks++ % REFRESH_TICKS == 0)
    {
      int old_w = s->source.width, old_h = s->source.height;

      if (!screencast_capture_refresh (s->capture, &s->source))
        {
          notify_closed (s);
          return G_SOURCE_REMOVE;
        }

      if (s->source.width != old_w || s->source.height != old_h)
        update_format_param (s);
    }

  if (s->negotiated_width != s->source.width || s->negotiated_height != s->source.height)
    return G_SOURCE_CONTINUE;

  dirty = screencast_capture_poll_damage (s->capture, &s->source);
  if (dirty || g_get_monotonic_time () - s->last_frame_us >= KEEPALIVE_US)
    produce_frame (s);

  return s->closed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

/* --- Public API ------------------------------------------------------- */

ScreenCastStream *
screencast_stream_new (ScreenCastCapture *c,
                       const ScreenCastSource *source,
                       gboolean embed_cursor,
                       GError **error)
{
  ScreenCastStream *s;
  gint64 deadline;
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  const struct spa_pod *params[1];
  int res;

  pw_init (NULL, NULL);

  s = g_new0 (ScreenCastStream, 1);
  s->capture = c;
  s->source = *source;
  s->source.name = g_strdup (source->name);
  s->source.app_id = NULL;
  s->embed_cursor = embed_cursor;
  s->node_id = SPA_ID_INVALID;

  s->loop = pw_loop_new (NULL);
  s->context = s->loop ? pw_context_new (s->loop, NULL, 0) : NULL;
  s->core = s->context ? pw_context_connect (s->context, NULL, 0) : NULL;
  if (!s->core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not connect to PipeWire: %s", g_strerror (errno));
      goto fail;
    }

  pw_loop_enter (s->loop);

  /* No explicit media.class: a producing stream defaults to Stream/Output/Video,
   * which is what session managers link browsers to by node id. */
  s->stream = pw_stream_new (s->core, "xapp-screencast",
                             pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                                                PW_KEY_MEDIA_CATEGORY, "Capture",
                                                PW_KEY_MEDIA_ROLE, "Screen",
                                                PW_KEY_NODE_NAME, "xapp-screencast",
                                                PW_KEY_NODE_DESCRIPTION, source->name ? source->name : "Screen",
                                                NULL));
  if (!s->stream)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create PipeWire stream");
      goto fail;
    }

  pw_stream_add_listener (s->stream, &s->stream_listener, &stream_events, s);

  params[0] = spa_pod_builder_add_object (
      &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_BGRx),
      SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle (&SPA_RECTANGLE (s->source.width, s->source.height)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&SPA_FRACTION (0, 1)),
      SPA_FORMAT_VIDEO_maxFramerate,
      SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (MAX_FRAMERATE, 1),
                                     &SPA_FRACTION (1, 1),
                                     &SPA_FRACTION (MAX_FRAMERATE, 1)));

  res = pw_stream_connect (s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                           PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
                           params, 1);
  if (res < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not connect PipeWire stream: %s", spa_strerror (res));
      goto fail;
    }

  /* The node id is what the portal returns, so wait for it */
  deadline = g_get_monotonic_time () + CONNECT_TIMEOUT_US;
  while (s->node_id == SPA_ID_INVALID && !s->failed && g_get_monotonic_time () < deadline)
    pw_loop_iterate (s->loop, 50);

  if (s->node_id == SPA_ID_INVALID)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           "PipeWire stream did not become ready");
      goto fail;
    }

  s->loop_source = pw_loop_source_new (s->loop);
  g_source_attach (s->loop_source, NULL);
  s->tick_id = g_timeout_add (TICK_MS, on_tick, s);

  return s;

fail:
  screencast_stream_destroy (s);
  return NULL;
}

guint32
screencast_stream_get_node_id (ScreenCastStream *stream)
{
  return stream->node_id;
}

void
screencast_stream_set_closed_callback (ScreenCastStream *stream,
                                       ScreenCastStreamClosedFunc cb,
                                       gpointer user_data)
{
  stream->closed_cb = cb;
  stream->closed_data = user_data;
}

void
screencast_stream_destroy (ScreenCastStream *s)
{
  if (!s)
    return;

  if (s->tick_id)
    g_source_remove (s->tick_id);

  if (s->loop_source)
    {
      g_source_destroy (s->loop_source);
      g_source_unref (s->loop_source);
    }

  if (s->stream)
    pw_stream_destroy (s->stream);
  if (s->core)
    pw_core_disconnect (s->core);
  if (s->context)
    pw_context_destroy (s->context);
  if (s->loop)
    {
      pw_loop_leave (s->loop);
      pw_loop_destroy (s->loop);
    }

  g_free (s->source.name);
  g_free (s);
}
