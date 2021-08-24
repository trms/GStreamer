/* GStreamer
 * Copyright (C) 2011 Alessandro Decina <alessandro.d@gmail.com>
 * Copyright (C) 2017 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2021 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-hlswebvttsink
 * @title: hlswebvttsink
 *
 * HTTP Live Streaming sink/server for WebVTT
 *
 * Since: 1.20
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsthlselements.h"
#include "gstm3u8playlist.h"
#include "gsthlswebvttsink.h"
#include <gst/video/video.h>
#include <memory.h>
#include <string.h>

#include <gio/gio.h>

GST_DEBUG_CATEGORY_STATIC (gst_hls_webvtt_sink_debug);
#define GST_CAT_DEFAULT gst_hls_webvtt_sink_debug

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_PLAYLIST_LOCATION,
  PROP_PLAYLIST_ROOT,
  PROP_MAX_FILES,
  PROP_TARGET_DURATION,
  PROP_PLAYLIST_LENGTH,
  PROP_MPEGTS_TIME_OFFSET,
};

enum
{
  SIGNAL_GET_PLAYLIST_STREAM,
  SIGNAL_GET_FRAGMENT_STREAM,
  SIGNAL_DELETE_FRAGMENT,
  SIGNAL_LAST
};

static guint signals[SIGNAL_LAST];

#define DEFAULT_LOCATION "segment%05d.webvtt"
#define DEFAULT_PLAYLIST_LOCATION "playlist.m3u8"
#define DEFAULT_PLAYLIST_ROOT NULL
#define DEFAULT_MAX_FILES 10
#define DEFAULT_TARGET_DURATION 15
#define DEFAULT_PLAYLIST_LENGTH 5
#define DEFAULT_TIMESTAMP_MAP_MPEGTS 324000000

#define GST_M3U8_PLAYLIST_VERSION 3

struct _GstHlsWebvttSink
{
  GstBaseSink parent;

  gchar *location;
  gchar *playlist_location;
  gchar *playlist_root;
  guint playlist_length;
  GstM3U8Playlist *playlist;

  guint index;
  GstClockTime last_running_time;
  gint max_files;
  gint target_duration;
  GstClockTime target_duration_ns;
  guint64 mpegts_time_offset;
  gchar *timestamp_map;

  GOutputStream *fragment_stream;
  GCancellable *cancellable;

  gchar *current_location;
  GQueue old_locations;

  GstM3U8PlaylistRenderState state;
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-subtitle-vtt-fragmented"));

#define gst_hls_webvtt_sink_parent_class parent_class
G_DEFINE_TYPE (GstHlsWebvttSink, gst_hls_webvtt_sink, GST_TYPE_BASE_SINK);
#define _do_init \
  hls_element_init (plugin); \
  GST_DEBUG_CATEGORY_INIT (gst_hls_webvtt_sink_debug, "hlswebvttsink", 0, \
  "hlswebvttsink");

GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (hlswebvttsink, "hlswebvttsink",
    GST_RANK_NONE, GST_TYPE_HLS_WEBVTT_SINK, _do_init);

static void gst_hls_webvtt_sink_finalize (GObject * object);
static void gst_hls_webvtt_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * spec);
static void gst_hls_webvtt_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * spec);

static gboolean gst_hls_webvtt_sink_start (GstBaseSink * sink);
static gboolean gst_hls_webvtt_sink_stop (GstBaseSink * sink);
static gboolean gst_hls_webvtt_sink_unlock (GstBaseSink * sink);
static gboolean gst_hls_webvtt_sink_unlock_stop (GstBaseSink * sink);
static gboolean gst_hls_webvtt_sink_event (GstBaseSink * sink,
    GstEvent * event);
static GstFlowReturn gst_hls_webvtt_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);

static GOutputStream *gst_hls_webvtt_sink_get_playlist_stream (GstHlsWebvttSink
    * self, const gchar * location);
static GOutputStream *gst_hls_webvtt_sink_get_fragment_stream (GstHlsWebvttSink
    * self, const gchar * location);

static void
gst_hls_webvtt_sink_class_init (GstHlsWebvttSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->finalize = gst_hls_webvtt_sink_finalize;
  gobject_class->set_property = gst_hls_webvtt_sink_set_property;
  gobject_class->get_property = gst_hls_webvtt_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Location",
          "Location of the file to write", DEFAULT_LOCATION,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_LOCATION,
      g_param_spec_string ("playlist-location", "Playlist Location",
          "Location of the playlist to write", DEFAULT_PLAYLIST_LOCATION,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_ROOT,
      g_param_spec_string ("playlist-root", "Playlist Root",
          "Location of the playlist to write", DEFAULT_PLAYLIST_ROOT,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_FILES,
      g_param_spec_uint ("max-files", "Max files",
          "Maximum number of files to keep on disk. Once the maximum is reached,"
          "old files start to be deleted to make room for new ones.", 0,
          G_MAXUINT, DEFAULT_MAX_FILES,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_TARGET_DURATION,
      g_param_spec_uint ("target-duration", "Target duration",
          "The target duration in seconds of a segment/file. "
          "(0 - disabled, useful for management of segment duration by the "
          "streaming server)", 0, G_MAXUINT, DEFAULT_TARGET_DURATION,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PLAYLIST_LENGTH,
      g_param_spec_uint ("playlist-length", "Playlist length",
          "Length of HLS playlist. To allow players to conform to section 6.3.3 "
          "of the HLS specification, this should be at least 3. If set to 0, "
          "the playlist will be infinite.", 0, G_MAXUINT,
          DEFAULT_PLAYLIST_LENGTH,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MPEGTS_TIME_OFFSET,
      g_param_spec_uint64 ("mpegts-time-offset", "MPEG TS Time Offset",
          "Time offset corresponding to the running time zero in MPEG TS time "
          "(i.e., 90khz clock base). Default is 324000000 "
          "(1 hour, 60 * 60 * 90000) which is identical to the offset used in "
          "mpegtsmux element",
          0, G_MAXUINT64, DEFAULT_TIMESTAMP_MAP_MPEGTS,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_GET_PLAYLIST_STREAM] =
      g_signal_new_class_handler ("get-playlist-stream",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_CALLBACK (gst_hls_webvtt_sink_get_playlist_stream),
      g_signal_accumulator_first_wins, NULL, NULL, G_TYPE_OUTPUT_STREAM, 1,
      G_TYPE_STRING);

  signals[SIGNAL_GET_FRAGMENT_STREAM] =
      g_signal_new_class_handler ("get-fragment-stream",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_CALLBACK (gst_hls_webvtt_sink_get_fragment_stream),
      g_signal_accumulator_first_wins, NULL, NULL, G_TYPE_OUTPUT_STREAM, 1,
      G_TYPE_STRING);

  signals[SIGNAL_DELETE_FRAGMENT] =
      g_signal_new ("delete-fragment", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_set_static_metadata (element_class,
      "HTTP Live Streaming sink for WebVTT", "Sink",
      "HTTP Live Streaming sink for WebVTT",
      "Seungha Yang <seungha@centricular.com>");

  basesink_class->start = GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_stop);
  basesink_class->unlock = GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_unlock);
  basesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_unlock_stop);
  basesink_class->event = GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_event);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_hls_webvtt_sink_render);
}

static void
gst_hls_webvtt_sink_init (GstHlsWebvttSink * self)
{
  self->location = g_strdup (DEFAULT_LOCATION);
  self->playlist_location = g_strdup (DEFAULT_PLAYLIST_LOCATION);
  self->playlist_root = g_strdup (DEFAULT_PLAYLIST_ROOT);
  self->playlist_length = DEFAULT_PLAYLIST_LENGTH;
  self->max_files = DEFAULT_MAX_FILES;
  self->target_duration = DEFAULT_TARGET_DURATION;
  self->target_duration_ns = DEFAULT_TARGET_DURATION * GST_SECOND;
  self->mpegts_time_offset = DEFAULT_TIMESTAMP_MAP_MPEGTS;

  self->cancellable = g_cancellable_new ();

  g_queue_init (&self->old_locations);
}

static void
gst_hls_webvtt_sink_finalize (GObject * object)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (object);

  g_free (self->location);
  g_free (self->playlist_location);
  g_free (self->playlist_root);
  g_clear_pointer (&self->playlist, gst_m3u8_playlist_free);
  g_free (self->timestamp_map);
  g_free (self->current_location);

  g_object_unref (self->cancellable);

  g_queue_foreach (&self->old_locations, (GFunc) g_free, NULL);
  g_queue_clear (&self->old_locations);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_hls_webvtt_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_free (self->location);
      self->location = g_value_dup_string (value);
      break;
    case PROP_PLAYLIST_LOCATION:
      g_free (self->playlist_location);
      self->playlist_location = g_value_dup_string (value);
      break;
    case PROP_PLAYLIST_ROOT:
      g_free (self->playlist_root);
      self->playlist_root = g_value_dup_string (value);
      break;
    case PROP_MAX_FILES:
      self->max_files = g_value_get_uint (value);
      break;
    case PROP_TARGET_DURATION:
      self->target_duration = g_value_get_uint (value);
      self->target_duration_ns = self->target_duration * GST_SECOND;
      break;
    case PROP_PLAYLIST_LENGTH:
      self->playlist_length = g_value_get_uint (value);
      break;
    case PROP_MPEGTS_TIME_OFFSET:
      self->mpegts_time_offset = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_hls_webvtt_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, self->location);
      break;
    case PROP_PLAYLIST_LOCATION:
      g_value_set_string (value, self->playlist_location);
      break;
    case PROP_PLAYLIST_ROOT:
      g_value_set_string (value, self->playlist_root);
      break;
    case PROP_MAX_FILES:
      g_value_set_uint (value, self->max_files);
      break;
    case PROP_TARGET_DURATION:
      g_value_set_uint (value, self->target_duration);
      break;
    case PROP_PLAYLIST_LENGTH:
      g_value_set_uint (value, self->playlist_length);
      break;
    case PROP_MPEGTS_TIME_OFFSET:
      g_value_set_uint64 (value, self->mpegts_time_offset);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GOutputStream *
gst_hls_webvtt_sink_get_playlist_stream (GstHlsWebvttSink * self,
    const gchar * location)
{
  GFile *file = g_file_new_for_path (location);
  GOutputStream *ostream;
  GError *err = NULL;

  ostream =
      G_OUTPUT_STREAM (g_file_replace (file, NULL, FALSE,
          G_FILE_CREATE_REPLACE_DESTINATION, NULL, &err));
  if (!ostream) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
        (("Got no output stream for playlist '%s': %s."), location,
            err->message), (NULL));
    g_clear_error (&err);
  }

  g_object_unref (file);

  return ostream;
}

static GOutputStream *
gst_hls_webvtt_sink_get_fragment_stream (GstHlsWebvttSink * self,
    const gchar * location)
{
  GFile *file = g_file_new_for_path (location);
  GOutputStream *ostream;
  GError *err = NULL;

  ostream =
      G_OUTPUT_STREAM (g_file_replace (file, NULL, FALSE,
          G_FILE_CREATE_REPLACE_DESTINATION, NULL, &err));
  if (!ostream) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
        (("Got no output stream for fragment '%s': %s."), location,
            err->message), (NULL));
    g_clear_error (&err);
  }

  g_object_unref (file);

  return ostream;
}

static gboolean
gst_hls_webvtt_sink_start (GstBaseSink * sink)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);

  self->index = 0;
  self->last_running_time = GST_CLOCK_TIME_NONE;

  g_clear_pointer (&self->playlist, gst_m3u8_playlist_free);
  g_clear_pointer (&self->timestamp_map, g_free);
  g_clear_pointer (&self->current_location, g_free);

  /* Convering uint64 to float is always problematic. Since we are supposed
   * to producing equal (and integer) duration which is the same as
   * target-duration apart from the last fragment, %.3f should be fine */
  self->playlist =
      gst_m3u8_playlist_new_full (GST_M3U8_PLAYLIST_VERSION,
      self->playlist_length, "%.3f");

  g_queue_foreach (&self->old_locations, (GFunc) g_free, NULL);
  g_queue_clear (&self->old_locations);

  self->state = GST_M3U8_PLAYLIST_RENDER_INIT;

  return TRUE;
}

static void
gst_hls_webvtt_sink_write_playlist (GstHlsWebvttSink * self)
{
  gchar *playlist_content;
  GError *error = NULL;
  GOutputStream *stream = NULL;
  gsize bytes_to_write;

  g_signal_emit (self, signals[SIGNAL_GET_PLAYLIST_STREAM], 0,
      self->playlist_location, &stream);
  if (!stream) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
        (("Got no output stream for playlist '%s'."), self->playlist_location),
        (NULL));
    return;
  }

  playlist_content = gst_m3u8_playlist_render (self->playlist);
  bytes_to_write = strlen (playlist_content);
  if (!g_output_stream_write_all (stream, playlist_content, bytes_to_write,
          NULL, NULL, &error)) {
    GST_ERROR ("Failed to write playlist: %s", error->message);
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
        (("Failed to write playlist '%s'."), error->message), (NULL));
  } else if (!g_output_stream_flush (stream, self->cancellable, &error)) {
    GST_WARNING_OBJECT (self, "Failed to flush stream");
  }

  g_clear_error (&error);

  g_free (playlist_content);
  g_object_unref (stream);
}

static gboolean
gst_hls_webvtt_sink_stop (GstBaseSink * sink)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);

  g_clear_object (&self->fragment_stream);

  if (self->playlist && (self->state & GST_M3U8_PLAYLIST_RENDER_STARTED) &&
      !(self->state & GST_M3U8_PLAYLIST_RENDER_ENDED)) {
    self->playlist->end_list = TRUE;
    gst_hls_webvtt_sink_write_playlist (self);
  }

  g_clear_pointer (&self->playlist, gst_m3u8_playlist_free);
  g_clear_pointer (&self->timestamp_map, g_free);

  return TRUE;
}

static gboolean
gst_hls_webvtt_sink_unlock (GstBaseSink * sink)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);

  g_cancellable_cancel (self->cancellable);

  return TRUE;
}

static gboolean
gst_hls_webvtt_sink_unlock_stop (GstBaseSink * sink)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  return TRUE;
}

static gboolean
gst_hls_webvtt_sink_event (GstBaseSink * sink, GstEvent * event)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;
      gst_event_parse_segment (event, &segment);

      if (segment->format != GST_FORMAT_TIME) {
        GST_WARNING_OBJECT (self, "Only time format segment is allowed");
        gst_event_unref (event);
        return FALSE;
      }

      GST_DEBUG_OBJECT (self, "New segment %" GST_SEGMENT_FORMAT, segment);
      break;
    }
    case GST_EVENT_EOS:
    {
      self->playlist->end_list = TRUE;
      gst_hls_webvtt_sink_write_playlist (self);
      self->state |= GST_M3U8_PLAYLIST_RENDER_ENDED;
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}

static gboolean
schedule_next_key_unit (GstHlsWebvttSink * self)
{
  GstClockTime running_time;
  GstEvent *event;

  if (self->target_duration == 0)
    return TRUE;

  running_time = self->last_running_time + self->target_duration_ns;
  GST_INFO_OBJECT (self, "sending upstream force-key-unit, index %d "
      "now %" GST_TIME_FORMAT " target %" GST_TIME_FORMAT,
      self->index + 1, GST_TIME_ARGS (self->last_running_time),
      GST_TIME_ARGS (running_time));

  event = gst_video_event_new_upstream_force_key_unit (running_time,
      TRUE, self->index + 1);

  if (!gst_pad_push_event (GST_BASE_SINK_PAD (self), event)) {
    GST_ERROR_OBJECT (self, "Failed to push upstream force key unit event");
    return FALSE;
  }

  return TRUE;
}

static void
gst_hls_webvtt_sink_timestamp_to_string (GstClockTime timestamp, GString * str)
{
  guint h, m, s, ms;

  h = timestamp / (3600 * GST_SECOND);

  timestamp -= h * 3600 * GST_SECOND;
  m = timestamp / (60 * GST_SECOND);

  timestamp -= m * 60 * GST_SECOND;
  s = timestamp / GST_SECOND;

  timestamp -= s * GST_SECOND;
  ms = timestamp / GST_MSECOND;

  g_string_append_printf (str, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

#define GSTTIME_TO_MPEGTIME(time) \
    gst_util_uint64_scale (time, 90000, GST_SECOND)

static GstBuffer *
gst_hls_webvtt_sink_insert_timestamp_map (GstHlsWebvttSink * self,
    GstBuffer * buf, GstClockTime running_time)
{
  /* Minimal validation */
  static const gchar webvtt_bom_hdr[] = {
    0xef, 0xbb, 0xbf, 'W', 'E', 'B', 'V', 'T', 'T'
  };
  static const gchar webvtt_hdr[] = {
    'W', 'E', 'B', 'V', 'T', 'T'
  };
  GstBuffer *header_buf = NULL;
  GstMapInfo map;
  gchar *next_line = NULL;
  gsize next_line_pos = 0;
  GString *str = NULL;
  gsize len;

  if (!self->timestamp_map) {
    GString *s = g_string_new ("X-TIMESTAMP-MAP=MPEGTS:");
    guint64 running_time_in_mpegts;

    /* Calculate mpegts time corresponding to the current buffer running time */
    running_time_in_mpegts = GSTTIME_TO_MPEGTIME (running_time);
    running_time_in_mpegts += self->mpegts_time_offset;

    /* Then pick the 33 bits to cover rollover case */
    running_time_in_mpegts &= 0x1ffffffff;

    g_string_append_printf (s,
        "%" G_GUINT64_FORMAT ",LOCAL:", running_time_in_mpegts);
    /* XXX: Assume written webvtt cue timestamp is equal to buffer timestmap */
    gst_hls_webvtt_sink_timestamp_to_string (GST_BUFFER_PTS (buf), s);

    self->timestamp_map = g_string_free (s, FALSE);
    GST_INFO_OBJECT (self,
        "segment %" GST_SEGMENT_FORMAT ", first buffer pts: %"
        GST_TIME_FORMAT ", running time %" GST_TIME_FORMAT ", timestamp map %s",
        &GST_BASE_SINK_CAST (self)->segment,
        GST_TIME_ARGS (GST_BUFFER_PTS (buf)), GST_TIME_ARGS (running_time),
        self->timestamp_map);
  }

  if (!gst_buffer_map (buf, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map header buffer for reading");
    gst_buffer_unref (buf);

    return NULL;
  }

  if (map.size < sizeof (webvtt_hdr))
    goto too_short;

  if (memcmp (map.data, webvtt_hdr, sizeof (webvtt_hdr)) != 0) {
    if (map.size < sizeof (webvtt_bom_hdr))
      goto invalid_header;

    if (memcmp (map.data, webvtt_bom_hdr, sizeof (webvtt_bom_hdr)) != 0)
      goto invalid_header;
  }

  len = map.size;
  if (map.data[map.size - 1] == '\0')
    len--;

  str = g_string_new_len (map.data, len);

  /* Find the first WebVTT line terminator CRLF, LF or CR */
  next_line = strstr (map.data, "\r\n");
  if (next_line)
    next_line_pos = (next_line - map.data) + 2;

  if (!next_line_pos) {
    next_line = strchr (map.data, '\n');
    if (next_line)
      next_line_pos = (next_line - map.data) + 1;
  }

  if (!next_line_pos) {
    next_line = strchr (map.data, '\r');
    if (next_line)
      next_line_pos = (next_line - map.data) + 1;
  }
  gst_buffer_unmap (buf, &map);

  if (!next_line_pos) {
    GST_WARNING_OBJECT (self, "Failed to find WebVTT line terminator");
    g_string_append_c (str, '\n');
    g_string_append (str, self->timestamp_map);
    g_string_append_c (str, '\n');
  } else {
    g_string_insert_len (str, next_line_pos, self->timestamp_map, -1);
    g_string_insert_c (str, next_line_pos + strlen (self->timestamp_map), '\n');
  }

out:
  len = str->len;
  header_buf = gst_buffer_new_wrapped (g_string_free (str, FALSE), len);

  /* Copy timestamp and flags */
  GST_BUFFER_PTS (header_buf) = GST_BUFFER_PTS (buf);
  GST_BUFFER_DTS (header_buf) = GST_BUFFER_DTS (buf);
  GST_BUFFER_DURATION (header_buf) = GST_BUFFER_DURATION (buf);
  GST_BUFFER_FLAGS (header_buf) = GST_BUFFER_FLAGS (buf);

  gst_buffer_unref (buf);

  return header_buf;

too_short:
  {
    GST_ERROR_OBJECT (self, "Header buffer size is too small");
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);

    return NULL;
  }

invalid_header:
  {
    GST_ERROR_OBJECT (self, "Invalid WebVTT header");
    gst_buffer_unmap (buf, &map);
    gst_buffer_unref (buf);

    return NULL;
  }
}

static GOutputStream *
get_fragment_stream (GstHlsWebvttSink * self, guint fragment_id)
{
  GOutputStream *stream = NULL;
  gchar *location;

  location = g_strdup_printf (self->location, fragment_id);
  g_signal_emit (self, signals[SIGNAL_GET_FRAGMENT_STREAM], 0, location,
      &stream);

  g_clear_pointer (&self->current_location, g_free);
  if (!stream) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
        (("Got no output stream for fragment '%s'."), location), (NULL));
  } else {
    self->current_location = g_steal_pointer (&location);
  }

  g_free (location);

  return stream;
}

static GstFlowReturn
gio_error_to_gst (GstHlsWebvttSink * self, GError ** err)
{
  GstFlowReturn ret = GST_FLOW_ERROR;

  if (g_error_matches (*err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    GST_DEBUG_OBJECT (self, "Operation cancelled");
    ret = GST_FLOW_FLUSHING;
  } else {
    GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
        ("Could not write to stream: %s", *err ? GST_STR_NULL ((*err)->message)
            : "Unknown"));
  }

  g_clear_error (err);

  return ret;
}

static GstFlowReturn
gst_hls_webvtt_sink_advance_playlist (GstHlsWebvttSink * self,
    GstClockTime running_time)
{
  gchar *entry_location;
  GstClockTime duration;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!self->current_location) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE, (NULL),
        ("Fragment closed without knowing its location"));
    return GST_FLOW_ERROR;
  }

  if (!self->playlist_root) {
    entry_location = g_path_get_basename (self->current_location);
  } else {
    gchar *name = g_path_get_basename (self->current_location);
    entry_location = g_build_filename (self->playlist_root, name, NULL);
    g_free (name);
  }

  duration = running_time - self->last_running_time;

  gst_m3u8_playlist_add_entry (self->playlist, entry_location,
      NULL, duration, self->index, FALSE);
  g_free (entry_location);

  self->last_running_time = running_time;
  self->index++;

  gst_hls_webvtt_sink_write_playlist (self);
  self->state |= GST_M3U8_PLAYLIST_RENDER_STARTED;

  g_queue_push_tail (&self->old_locations, g_strdup (self->current_location));
  if (self->max_files > 0) {
    while (g_queue_get_length (&self->old_locations) > self->max_files) {
      gchar *old_location = g_queue_pop_head (&self->old_locations);

      if (g_signal_has_handler_pending (self,
              signals[SIGNAL_DELETE_FRAGMENT], 0, FALSE)) {
        g_signal_emit (self, signals[SIGNAL_DELETE_FRAGMENT], 0, old_location);
      } else {
        GFile *file = g_file_new_for_path (old_location);
        GError *err = NULL;

        if (!g_file_delete (file, NULL, &err)) {
          GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
              (("Failed to delete fragment file '%s': %s."),
                  old_location, err->message), (NULL));
          g_clear_error (&err);
          ret = GST_FLOW_ERROR;
        }
        g_object_unref (file);
      }

      g_free (old_location);
    }
  }

  g_clear_pointer (&self->current_location, g_free);

  return ret;
}

static GstFlowReturn
gst_hls_webvtt_sink_render (GstBaseSink * sink, GstBuffer * buf)
{
  GstHlsWebvttSink *self = GST_HLS_WEBVTT_SINK (sink);
  GstMapInfo info;
  gboolean write_ret;
  GError *err = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *render_buf;

  if (!GST_BUFFER_PTS_IS_VALID (buf)) {
    GST_ERROR_OBJECT (self, "Invalid timestamp");

    return GST_FLOW_ERROR;
  }

  render_buf = gst_buffer_ref (buf);

  if (GST_BUFFER_FLAG_IS_SET (render_buf, GST_BUFFER_FLAG_HEADER) ||
      !GST_BUFFER_FLAG_IS_SET (render_buf, GST_BUFFER_FLAG_DELTA_UNIT)) {
    GstClockTime running_time;

    running_time = gst_segment_to_running_time (&sink->segment,
        GST_FORMAT_TIME, GST_BUFFER_PTS (render_buf));

    render_buf = gst_hls_webvtt_sink_insert_timestamp_map (self,
        render_buf, running_time);

    if (!render_buf)
      return GST_FLOW_ERROR;

    if (!self->fragment_stream) {
      /* This is the first buffer */
      self->last_running_time = running_time;
      schedule_next_key_unit (self);
    } else {
      if (!g_output_stream_flush (self->fragment_stream, self->cancellable,
              &err)) {
        GST_WARNING_OBJECT (self, "Failed to flush fragment stream, %s",
            err->message);
        g_clear_error (&err);
      }

      ret = gst_hls_webvtt_sink_advance_playlist (self, running_time);
      if (ret != GST_FLOW_OK)
        return ret;

      schedule_next_key_unit (self);
    }

    g_clear_object (&self->fragment_stream);
    self->fragment_stream = get_fragment_stream (self, self->index);
  }

  if (!self->fragment_stream) {
    GST_ERROR_OBJECT (self, "No configured fragment stream");
    gst_buffer_unref (render_buf);

    return GST_FLOW_ERROR;
  }

  gst_buffer_map (render_buf, &info, GST_MAP_READ);
  write_ret = g_output_stream_write_all (self->fragment_stream, info.data,
      info.size, NULL, self->cancellable, &err);
  gst_buffer_unmap (render_buf, &info);
  gst_buffer_unref (render_buf);

  if (write_ret)
    return GST_FLOW_OK;

  return gio_error_to_gst (self, &err);
}
