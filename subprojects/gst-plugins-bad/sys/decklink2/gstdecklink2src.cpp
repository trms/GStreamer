/*
 * GStreamer
 * Copyright (C) 2023 Seungha Yang <seungha@centricular.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdecklink2src.h"
#include "gstdecklink2utils.h"
#include "gstdecklink2object.h"

#include <mutex>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_src_debug);
#define GST_CAT_DEFAULT gst_decklink2_src_debug

enum
{
  PROP_0,
  PROP_MODE,
  PROP_DEVICE_NUMBER,
  PROP_PERSISTENT_ID,
  PROP_VIDEO_CONNECTION,
  PROP_AUDIO_CONNECTION,
  PROP_VIDEO_FORMAT,
  PROP_AUDIO_CHANNELS,
  PROP_PROFILE_ID,
  PROP_TIMECODE_FORMAT,
  PROP_OUTPUT_CC,
  PROP_OUTPUT_AFD_BAR,
  PROP_BUFFER_SIZE,
  PROP_SIGNAL,
  PROP_SKIP_FIRST_TIME,
  PROP_DESYNC_THRESHOLD,
};

#define DEFAULT_MODE                bmdModeUnknown
#define DEFAULT_DEVICE_NUMBER       0
#define DEFAULT_PERSISTENT_ID       -1
#define DEFAULT_VIDEO_CONNECTION    bmdVideoConnectionUnspecified
#define DEFAULT_AUDIO_CONNECTION    bmdAudioConnectionUnspecified
#define DEFAULT_VIDEO_FORMAT        bmdFormat8BitYUV
#define DEFAULT_PROFILE_ID          bmdProfileDefault
#define DEFAULT_TIMECODE_FORMAT     bmdTimecodeRP188Any
#define DEFAULT_OUTPUT_CC           FALSE
#define DEFAULT_OUTPUT_AFD_BAR      FALSE
#define DEFAULT_BUFFER_SIZE         5
#define DEFAULT_AUDIO_CHANNELS      GST_DECKLINK2_AUDIO_CHANNELS_2
#define DEFAULT_SKIP_FIRST_TIME     0
#define DEFAULT_DESYNC_THRESHOLD    (250 * GST_MSECOND)

enum
{
  /* actions */
  SIGNAL_RESTART,

  SIGNAL_LAST,
};

static guint gst_decklink2_src_signals[SIGNAL_LAST] = { 0, };

struct GstDeckLink2SrcPrivate
{
  std::mutex lock;
};

struct _GstDeckLink2Src
{
  GstPushSrc parent;

  GstDeckLink2SrcPrivate *priv;

  GstVideoInfo video_info;

  GstDeckLink2Input *input;
  GstDeckLink2DisplayMode selected_mode;
  GstCaps *selected_caps;
  gboolean is_gap_buf;

  gboolean running;

  /* properties */
  BMDDisplayMode display_mode;
  gint device_number;
  gint64 persistent_id;
  BMDVideoConnection video_conn;
  BMDAudioConnection audio_conn;
  BMDPixelFormat video_format;
  GstDeckLink2AudioChannels audio_channels;
  BMDProfileID profile_id;
  BMDTimecodeFormat timecode_format;
  gboolean output_cc;
  gboolean output_afd_bar;
  guint buffer_size;
  GstClockTime skip_first_time;
  GstClockTime desync_threshold;
};

static void gst_decklink2_src_finalize (GObject * object);
static void gst_decklink2_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_decklink2_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static GstCaps *gst_decklink2_src_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_decklink2_src_set_caps (GstBaseSrc * src, GstCaps * caps);
static gboolean gst_decklink2_src_query (GstBaseSrc * src, GstQuery * query);
static gboolean gst_decklink2_src_start (GstBaseSrc * src);
static gboolean gst_decklink2_src_stop (GstBaseSrc * src);
static gboolean gst_decklink2_src_unlock (GstBaseSrc * src);
static gboolean gst_decklink2_src_unlock_stop (GstBaseSrc * src);

static GstFlowReturn gst_decklink2_src_create (GstPushSrc * src,
    GstBuffer ** buffer);

#define gst_decklink2_src_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Src, gst_decklink2_src, GST_TYPE_PUSH_SRC);
GST_ELEMENT_REGISTER_DEFINE (decklink2src, "decklink2src",
    GST_RANK_NONE, GST_TYPE_DECKLINK2_SRC);

static void
gst_decklink2_src_class_init (GstDeckLink2SrcClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS (klass);
  GstCaps *templ_caps;

  object_class->finalize = gst_decklink2_src_finalize;
  object_class->set_property = gst_decklink2_src_set_property;
  object_class->get_property = gst_decklink2_src_get_property;

  gst_decklink2_src_install_properties (object_class);

  gst_decklink2_src_signals[SIGNAL_RESTART] =
      g_signal_new_class_handler ("restart", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_CALLBACK (gst_decklink2_src_restart), NULL, NULL, NULL, G_TYPE_NONE, 0);

  templ_caps = gst_decklink2_get_default_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref (templ_caps);

  gst_element_class_set_static_metadata (element_class,
      "Decklink2 Source", "Video/Audio/Source/Hardware", "Decklink2 Source",
      "Seungha Yang <seungha@centricular.com>");

  basesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_decklink2_src_get_caps);
  basesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_decklink2_src_set_caps);
  basesrc_class->query = GST_DEBUG_FUNCPTR (gst_decklink2_src_query);
  basesrc_class->start = GST_DEBUG_FUNCPTR (gst_decklink2_src_start);
  basesrc_class->stop = GST_DEBUG_FUNCPTR (gst_decklink2_src_stop);
  basesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_decklink2_src_unlock);
  basesrc_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_decklink2_src_unlock_stop);

  pushsrc_class->create = GST_DEBUG_FUNCPTR (gst_decklink2_src_create);

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_src_debug, "decklink2src",
      0, "decklink2src");

  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_MODE, (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_VIDEO_CONNECTION,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_AUDIO_CONNECTION,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_VIDEO_FORMAT,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_AUDIO_CHANNELS,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_PROFILE_ID,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_TIMECODE_FORMAT,
      (GstPluginAPIFlags) 0);
}

static void
gst_decklink2_src_init (GstDeckLink2Src * self)
{
  self->display_mode = DEFAULT_MODE;
  self->device_number = DEFAULT_DEVICE_NUMBER;
  self->persistent_id = DEFAULT_PERSISTENT_ID;
  self->video_conn = DEFAULT_VIDEO_CONNECTION;
  self->audio_conn = DEFAULT_AUDIO_CONNECTION;
  self->video_format = DEFAULT_VIDEO_FORMAT;
  self->profile_id = DEFAULT_PROFILE_ID;
  self->audio_channels = DEFAULT_AUDIO_CHANNELS;
  self->timecode_format = DEFAULT_TIMECODE_FORMAT;
  self->output_cc = DEFAULT_OUTPUT_CC;
  self->output_afd_bar = DEFAULT_OUTPUT_AFD_BAR;
  self->buffer_size = DEFAULT_BUFFER_SIZE;
  self->is_gap_buf = FALSE;
  self->desync_threshold = DEFAULT_DESYNC_THRESHOLD;

  self->priv = new GstDeckLink2SrcPrivate ();

  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
}

static void
gst_decklink2_src_finalize (GObject * object)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (object);

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_decklink2_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (object);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  switch (prop_id) {
    case PROP_MODE:
      self->display_mode = (BMDDisplayMode) g_value_get_enum (value);
      break;
    case PROP_DEVICE_NUMBER:
      self->device_number = g_value_get_int (value);
      break;
    case PROP_PERSISTENT_ID:
      self->persistent_id = g_value_get_int64 (value);
      break;
    case PROP_VIDEO_CONNECTION:
      self->video_conn = (BMDVideoConnection) g_value_get_enum (value);
      break;
    case PROP_AUDIO_CONNECTION:
      self->audio_conn = (BMDAudioConnection) g_value_get_enum (value);
      break;
    case PROP_VIDEO_FORMAT:
      self->video_format = (BMDPixelFormat) g_value_get_enum (value);
      break;
    case PROP_AUDIO_CHANNELS:
      self->audio_channels =
          (GstDeckLink2AudioChannels) g_value_get_enum (value);
      break;
    case PROP_PROFILE_ID:
      self->profile_id = (BMDProfileID) g_value_get_enum (value);
      break;
    case PROP_TIMECODE_FORMAT:
      self->timecode_format = (BMDTimecodeFormat) g_value_get_enum (value);
      break;
    case PROP_OUTPUT_CC:
      self->output_cc = g_value_get_boolean (value);
      break;
    case PROP_OUTPUT_AFD_BAR:
      self->output_afd_bar = g_value_get_boolean (value);
      break;
    case PROP_BUFFER_SIZE:
      self->buffer_size = g_value_get_uint (value);
      break;
    case PROP_SKIP_FIRST_TIME:
      self->skip_first_time = g_value_get_uint64 (value);
      break;
    case PROP_DESYNC_THRESHOLD:
      self->desync_threshold = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_decklink2_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (object);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, self->display_mode);
      break;
    case PROP_DEVICE_NUMBER:
      g_value_set_int (value, self->device_number);
      break;
    case PROP_PERSISTENT_ID:
      g_value_set_int64 (value, self->persistent_id);
      break;
    case PROP_VIDEO_CONNECTION:
      g_value_set_enum (value, self->video_conn);
      break;
    case PROP_AUDIO_CONNECTION:
      g_value_set_enum (value, self->audio_conn);
      break;
    case PROP_VIDEO_FORMAT:
      g_value_set_enum (value, self->video_format);
      break;
    case PROP_AUDIO_CHANNELS:
      g_value_set_enum (value, self->audio_channels);
      break;
    case PROP_PROFILE_ID:
      g_value_set_enum (value, self->profile_id);
      break;
    case PROP_TIMECODE_FORMAT:
      g_value_set_enum (value, self->timecode_format);
      break;
    case PROP_OUTPUT_CC:
      g_value_set_boolean (value, self->output_cc);
      break;
    case PROP_OUTPUT_AFD_BAR:
      g_value_set_boolean (value, self->output_afd_bar);
      break;
    case PROP_BUFFER_SIZE:
      g_value_set_uint (value, self->buffer_size);
      break;
    case PROP_SIGNAL:
    {
      gboolean has_signal = FALSE;
      if (self->input)
        has_signal = gst_decklink2_input_has_signal (self->input);

      g_value_set_boolean (value, has_signal);
      break;
    }
    case PROP_SKIP_FIRST_TIME:
      g_value_set_uint64 (value, self->skip_first_time);
      break;
    case PROP_DESYNC_THRESHOLD:
      g_value_set_uint64 (value, self->desync_threshold);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_decklink2_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  GstCaps *caps;
  GstCaps *ret;
  std::unique_lock < std::mutex > lk (priv->lock);

  if (!self->input) {
    lk.unlock ();
    return GST_BASE_SRC_CLASS (parent_class)->get_caps (src, filter);
  }

  if (self->selected_caps) {
    caps = gst_caps_ref (self->selected_caps);
  } else {
    caps = gst_decklink2_input_get_caps (self->input, self->display_mode,
        self->video_format);
  }

  if (!caps) {
    GST_WARNING_OBJECT (self, "Couldn't get caps");
    caps = gst_caps_new_empty ();
  }

  if (filter) {
    ret = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
  } else {
    ret = caps;
  }

  GST_DEBUG_OBJECT (self, "Returning caps %" GST_PTR_FORMAT, ret);

  return ret;
}

static gboolean
gst_decklink2_src_set_caps (GstBaseSrc * src, GstCaps * caps)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  BMDPixelFormat pixel_format;
  std::lock_guard < std::mutex > lk (priv->lock);

  GST_DEBUG_OBJECT (self, "Set caps %" GST_PTR_FORMAT, caps);

  if (!self->input) {
    GST_WARNING_OBJECT (self,
        "Couldn't accept caps without configured input object");
    return FALSE;
  }

  if (self->running)
    return TRUE;

  if (!gst_video_info_from_caps (&self->video_info, caps)) {
    GST_WARNING_OBJECT (self, "Invalid caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  if (!gst_decklink2_input_get_display_mode (self->input, &self->video_info,
          &self->selected_mode)) {
    GST_ERROR_OBJECT (self, "Not a supported caps");
    return FALSE;
  }

  gst_clear_caps (&self->selected_caps);
  pixel_format =
      gst_decklink2_pixel_format_from_video_format (GST_VIDEO_INFO_FORMAT
      (&self->video_info));
  self->selected_caps =
      gst_decklink2_input_get_caps (self->input, self->selected_mode.mode,
      pixel_format);

  if (!self->selected_caps) {
    GST_ERROR_OBJECT (self, "Couldn't get caps from selected mode");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_decklink2_src_query (GstBaseSrc * src, GstQuery * query)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      std::lock_guard < std::mutex > lk (priv->lock);
      gint fps_n, fps_d;
      GstClockTime min, max;
      if (self->selected_mode.fps_n > 0 && self->selected_mode.fps_d > 0) {
        fps_n = self->selected_mode.fps_n;
        fps_d = self->selected_mode.fps_d;
      } else {
        fps_n = 30;
        fps_d = 1;
      }

      min = gst_util_uint64_scale (GST_SECOND, fps_d, fps_n);
      max = self->buffer_size * min;
      gst_query_set_latency (query, TRUE, min, max);
      return TRUE;
    }
    default:
      break;
  }

  return GST_BASE_SRC_CLASS (parent_class)->query (src, query);
}

static gboolean
gst_decklink2_src_start (GstBaseSrc * src)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  self->running = FALSE;
  memset (&self->selected_mode, 0, sizeof (GstDeckLink2DisplayMode));

  gst_clear_caps (&self->selected_caps);
  self->input = gst_decklink2_acquire_input (self->device_number,
      self->persistent_id);

  if (!self->input) {
    GST_ERROR_OBJECT (self, "Couldn't acquire input object");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_decklink2_src_stop (GstBaseSrc * src)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (self->input) {
    gst_decklink2_input_stop (self->input);
    gst_decklink2_release_input (self->input);
    self->input = NULL;
  }

  gst_clear_caps (&self->selected_caps);
  memset (&self->selected_mode, 0, sizeof (GstDeckLink2DisplayMode));

  self->running = FALSE;

  return TRUE;
}

static gboolean
gst_decklink2_src_unlock (GstBaseSrc * src)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (self->input)
    gst_decklink2_input_set_flushing (self->input, TRUE);

  return TRUE;
}

static gboolean
gst_decklink2_src_unlock_stop (GstBaseSrc * src)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (self->input)
    gst_decklink2_input_set_flushing (self->input, FALSE);

  return TRUE;
}

static gboolean
gst_decklink2_src_run_unlocked (GstDeckLink2Src * self, gboolean auto_restart)
{
  HRESULT hr;
  GstDeckLink2InputVideoConfig video_config;
  GstDeckLink2InputAudioConfig audio_config;

  video_config.connection = self->video_conn;
  video_config.display_mode = self->selected_mode;
  video_config.pixel_format = self->video_format;
  video_config.auto_detect = self->display_mode == bmdModeUnknown;
  video_config.output_cc = self->output_cc;
  video_config.output_afd_bar = self->output_afd_bar;

  audio_config.connection = self->audio_conn;
  audio_config.sample_type = bmdAudioSampleType32bitInteger;
  audio_config.channels = self->audio_channels;

  hr = gst_decklink2_input_start (self->input, GST_ELEMENT (self),
      self->profile_id, self->buffer_size,
      auto_restart ? 0 : self->skip_first_time, &video_config, &audio_config);
  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (self, "Couldn't start stream, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  self->running = TRUE;
  return TRUE;
}

static gboolean
gst_decklink2_src_run (GstDeckLink2Src * self)
{
  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (self->running)
    return TRUE;

  if (!self->input) {
    GST_ERROR_OBJECT (self, "Input object was not configured");
    return FALSE;
  }

  return gst_decklink2_src_run_unlocked (self, FALSE);
}

static GstFlowReturn
gst_decklink2_src_create (GstPushSrc * src, GstBuffer ** buffer)
{
  GstDeckLink2Src *self = GST_DECKLINK2_SRC (src);
  GstBuffer *buf = nullptr;
  GstCaps *caps = nullptr;
  GstFlowReturn ret;
  GstDeckLink2SrcPrivate *priv = self->priv;
  gboolean is_gap_buf = FALSE;
  GstClockTimeDiff av_sync;
  guint retry_count = 0;
  gsize buf_size;

again:
  if (retry_count > 30) {
    GST_ERROR_OBJECT (self, "Too many buffers were dropped");
    return GST_FLOW_ERROR;
  }

  if (!gst_decklink2_src_run (self)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("Failed to start stream"));
    return GST_FLOW_ERROR;
  }

  ret = gst_decklink2_input_get_data (self->input, &buf, &caps, &av_sync);
  if (ret != GST_FLOW_OK) {
    if (ret == GST_DECKLINK2_INPUT_FLOW_STOPPED) {
      GST_DEBUG_OBJECT (self, "Input was stopped for restarting");
      retry_count++;
      goto again;
    }

    return ret;
  }

  if (!caps) {
    GST_WARNING_OBJECT (self, "Buffer without caps");
    gst_clear_buffer (&buf);
    retry_count++;
    goto again;
  }

  priv->lock.lock ();
  if (!gst_caps_is_equal (caps, self->selected_caps)) {
    GST_DEBUG_OBJECT (self, "Set updated caps %" GST_PTR_FORMAT, caps);
    gst_caps_replace (&self->selected_caps, caps);
    gst_video_info_from_caps (&self->video_info, caps);
    priv->lock.unlock ();
    if (!gst_pad_set_caps (GST_BASE_SRC_PAD (self), caps)) {
      GST_ERROR_OBJECT (self, "Couldn't set caps");
      gst_clear_buffer (&buf);
      gst_clear_caps (&caps);

      return GST_FLOW_NOT_NEGOTIATED;
    }
  } else {
    priv->lock.unlock ();
  }
  gst_clear_caps (&caps);

  if (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_GAP))
    is_gap_buf = TRUE;

  if (is_gap_buf != self->is_gap_buf) {
    self->is_gap_buf = is_gap_buf;
    g_object_notify (G_OBJECT (self), "signal");
  }

  buf_size = gst_buffer_get_size (buf);
  if (buf_size < self->video_info.size) {
    GST_WARNING_OBJECT (self, "Too small buffer size %" G_GSIZE_FORMAT
        " < %" G_GSIZE_FORMAT, buf_size, self->video_info.size);
    gst_clear_buffer (&buf);
    retry_count++;
    goto again;
  }

  std::unique_lock < std::mutex > lk (priv->lock);
  *buffer = buf;

  if (self->desync_threshold != 0 &&
      GST_CLOCK_TIME_IS_VALID (self->desync_threshold)) {
    GstClockTime diff;
    if (av_sync >= 0)
      diff = av_sync;
    else
      diff = -av_sync;

    GST_LOG_OBJECT (self, "Current AV sync %" GST_STIME_FORMAT,
        GST_STIME_ARGS (av_sync));

    if (diff >= self->desync_threshold) {
      GST_WARNING_OBJECT (self, "Large AV desync is detected, desync %"
          GST_STIME_FORMAT ", threshold %" GST_TIME_FORMAT,
          GST_STIME_ARGS (av_sync), GST_TIME_ARGS (self->desync_threshold));

      gst_decklink2_input_schedule_restart (self->input);
    }
  }

  return GST_FLOW_OK;
}

void
gst_decklink2_src_install_properties (GObjectClass * object_class)
{
  GParamFlags param_flags = (GParamFlags) (G_PARAM_READWRITE |
      GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_property (object_class, PROP_MODE,
      g_param_spec_enum ("mode", "Playback Mode",
          "Video Mode to use for playback",
          GST_TYPE_DECKLINK2_MODE, DEFAULT_MODE, param_flags));

  g_object_class_install_property (object_class, PROP_DEVICE_NUMBER,
      g_param_spec_int ("device-number", "Device number",
          "Output device instance to use", 0, G_MAXINT, DEFAULT_DEVICE_NUMBER,
          param_flags));

  g_object_class_install_property (object_class, PROP_PERSISTENT_ID,
      g_param_spec_int64 ("persistent-id", "Persistent id",
          "Output device instance to use. Higher priority than \"device-number\".",
          DEFAULT_PERSISTENT_ID, G_MAXINT64, DEFAULT_PERSISTENT_ID,
          param_flags));

  g_object_class_install_property (object_class, PROP_VIDEO_CONNECTION,
      g_param_spec_enum ("video-connection", "Video Connection",
          "Video input connection to use",
          GST_TYPE_DECKLINK2_VIDEO_CONNECTION, DEFAULT_VIDEO_CONNECTION,
          param_flags));

  g_object_class_install_property (object_class, PROP_AUDIO_CONNECTION,
      g_param_spec_enum ("audio-connection", "Audio Connection",
          "Audio input connection to use",
          GST_TYPE_DECKLINK2_AUDIO_CONNECTION, DEFAULT_AUDIO_CONNECTION,
          param_flags));

  g_object_class_install_property (object_class, PROP_VIDEO_FORMAT,
      g_param_spec_enum ("video-format", "Video format",
          "Video format type to use for playback",
          GST_TYPE_DECKLINK2_VIDEO_FORMAT, DEFAULT_VIDEO_FORMAT, param_flags));

  g_object_class_install_property (object_class, PROP_AUDIO_CHANNELS,
      g_param_spec_enum ("audio-channels", "Audio Channels",
          "Audio Channels",
          GST_TYPE_DECKLINK2_AUDIO_CHANNELS, DEFAULT_AUDIO_CHANNELS,
          param_flags));

  g_object_class_install_property (object_class, PROP_PROFILE_ID,
      g_param_spec_enum ("profile", "Profile",
          "Certain DeckLink devices such as the DeckLink 8K Pro, the DeckLink "
          "Quad 2 and the DeckLink Duo 2 support multiple profiles to "
          "configure the capture and playback behavior of its sub-devices."
          "For the DeckLink Duo 2 and DeckLink Quad 2, a profile is shared "
          "between any 2 sub-devices that utilize the same connectors. For the "
          "DeckLink 8K Pro, a profile is shared between all 4 sub-devices. Any "
          "sub-devices that share a profile are considered to be part of the "
          "same profile group."
          "DeckLink Duo 2 support configuration of the duplex mode of "
          "individual sub-devices.",
          GST_TYPE_DECKLINK2_PROFILE_ID, DEFAULT_PROFILE_ID, param_flags));

  g_object_class_install_property (object_class, PROP_TIMECODE_FORMAT,
      g_param_spec_enum ("timecode-format", "Timecode format",
          "Timecode format type to use for playback",
          GST_TYPE_DECKLINK2_TIMECODE_FORMAT, DEFAULT_TIMECODE_FORMAT,
          param_flags));

  g_object_class_install_property (object_class, PROP_OUTPUT_CC,
      g_param_spec_boolean ("output-cc", "Output Closed Caption",
          "Extract and output CC as GstMeta (if present)",
          DEFAULT_OUTPUT_CC, param_flags));

  g_object_class_install_property (object_class, PROP_OUTPUT_AFD_BAR,
      g_param_spec_boolean ("output-afd-bar", "Output AFD/Bar data",
          "Extract and output AFD/Bar as GstMeta (if present)",
          DEFAULT_OUTPUT_AFD_BAR, param_flags));

  g_object_class_install_property (object_class, PROP_BUFFER_SIZE,
      g_param_spec_uint ("buffer-size", "Buffer Size",
          "Size of internal buffer in number of video frames", 1,
          16, DEFAULT_BUFFER_SIZE, param_flags));

  g_object_class_install_property (object_class, PROP_SIGNAL,
      g_param_spec_boolean ("signal", "Signal",
          "True if there is a valid input signal available",
          FALSE, (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class, PROP_SKIP_FIRST_TIME,
      g_param_spec_uint64 ("skip-first-time", "Skip First Time",
          "Skip that much time of initial frames after starting", 0,
          G_MAXUINT64, DEFAULT_SKIP_FIRST_TIME,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class, PROP_DESYNC_THRESHOLD,
      g_param_spec_uint64 ("desync-threshold", "Desync Threshold",
          "Maximum allowed a/v desync threshold. "
          "If larger desync is detected, streaming will be restarted "
          "(0 = disable auto-restart)", 0,
          G_MAXUINT64, DEFAULT_DESYNC_THRESHOLD,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

void
gst_decklink2_src_restart (GstDeckLink2Src * self)
{
  g_return_if_fail (GST_IS_DECKLINK2_SRC (self));

  GstDeckLink2SrcPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (self->input && self->running) {
    GST_INFO_OBJECT (self, "Scheduling restart");
    gst_decklink2_input_schedule_restart (self->input);
  }
}
