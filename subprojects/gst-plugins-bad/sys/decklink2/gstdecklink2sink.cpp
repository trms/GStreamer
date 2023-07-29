/*
 * GStreamer
 * Copyright (C) 2021 Mathieu Duponchelle <mathieu@centricular.com>
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

#include "gstdecklink2sink.h"
#include "gstdecklink2utils.h"
#include "gstdecklink2object.h"
#include <string.h>
#include <mutex>

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_sink_debug);
#define GST_CAT_DEFAULT gst_decklink2_sink_debug

enum
{
  PROP_0,
  PROP_MODE,
  PROP_DEVICE_NUMBER,
  PROP_VIDEO_FORMAT,
  PROP_PROFILE_ID,
  PROP_TIMECODE_FORMAT,
  PROP_KEYER_MODE,
  PROP_KEYER_LEVEL,
  PROP_CC_LINE,
  PROP_AFD_BAR_LINE,
  PROP_MAPPING_FORMAT,
  PROP_PERSISTENT_ID,
  PROP_N_PREROLL_FRAMES,
  PROP_MIN_BUFFERED_FRAMES,
  PROP_MAX_BUFFERED_FRAMES,
  PROP_AUTO_RESTART,
  PROP_OUTPUT_STATS,
};

#define DEFAULT_MODE                bmdModeUnknown
#define DEFAULT_DEVICE_NUMBER       0
#define DEFAULT_PERSISTENT_ID       -1
#define DEFAULT_VIDEO_FORMAT        bmdFormat8BitYUV
#define DEFAULT_PROFILE_ID          bmdProfileDefault
#define DEFAULT_TIMECODE_FORMAT     bmdTimecodeRP188Any
#define DEFAULT_KEYER_MODE          GST_DECKLINK2_KEYER_MODE_OFF
#define DEFAULT_KEYER_LEVEL         255
#define DEFAULT_CC_LINE             0
#define DEFAULT_AFD_BAR_LINE        0
#define DEFAULT_MAPPING_FORMAT      GST_DECKLINK2_MAPPING_FORMAT_DEFAULT
#define DEFAULT_N_PREROLL_FRAMES    7
#define DEFAULT_MIN_BUFFERED_FRAMES 3
#define DEFAULT_MAX_BUFFERED_FRAMES 14
#define DEFAULT_AUTO_RESTART        FALSE

enum
{
  /* actions */
  SIGNAL_RESTART,

  SIGNAL_LAST,
};

static guint gst_decklink2_sink_signals[SIGNAL_LAST] = { 0, };

struct GstDeckLink2SinkPrivate
{
  std::mutex lock;
};

struct _GstDeckLink2Sink
{
  GstBaseSink parent;

  GstDeckLink2SinkPrivate *priv;

  GstDeckLink2Output *output;

  GstVideoInfo video_info;

  GstDeckLink2DisplayMode selected_mode;
  BMDAudioSampleType audio_sample_type;
  gint audio_channels;
  gboolean configured;

  GstBufferPool *fallback_pool;
  IDeckLinkVideoFrame *prepared_frame;
  BMDVideoOutputFlags output_flags;
  gboolean schedule_restart;

  /* properties */
  BMDDisplayMode display_mode;
  gint device_number;
  gint64 persistent_id;
  BMDPixelFormat video_format;
  BMDProfileID profile_id;
  BMDTimecodeFormat timecode_format;
  GstDeckLink2KeyerMode keyer_mode;
  gint keyer_level;
  gint caption_line;
  gint afd_bar_line;
  GstDeckLink2MappingFormat mapping_format;
  guint n_preroll_frames;
  guint min_buffered_frames;
  guint max_buffered_frames;
  gboolean auto_restart;

  GstDecklink2OutputStats stats;
};

static void gst_decklink2_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_decklink2_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_decklink2_sink_finalize (GObject * object);
static gboolean gst_decklink2_sink_query (GstBaseSink * sink, GstQuery * query);
static GstCaps *gst_decklink2_sink_get_caps (GstBaseSink * sink,
    GstCaps * filter);
static gboolean gst_decklink2_sink_set_caps (GstBaseSink * sink,
    GstCaps * caps);
static gboolean gst_decklink2_sink_propose_allocation (GstBaseSink * sink,
    GstQuery * query);
static gboolean gst_decklink2_sink_start (GstBaseSink * sink);
static gboolean gst_decklink2_sink_stop (GstBaseSink * sink);
static gboolean gst_decklink2_sink_unlock_stop (GstBaseSink * sink);
static GstFlowReturn gst_decklink2_sink_prepare (GstBaseSink * sink,
    GstBuffer * buffer);
static GstFlowReturn gst_decklink2_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static void gst_decklink2_sink_restart (GstDeckLink2Sink * self);

#define gst_decklink2_sink_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Sink, gst_decklink2_sink, GST_TYPE_BASE_SINK);
GST_ELEMENT_REGISTER_DEFINE (decklink2sink, "decklink2sink",
    GST_RANK_NONE, GST_TYPE_DECKLINK2_SINK);

static void
gst_decklink2_sink_class_init (GstDeckLink2SinkClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GParamFlags param_flags = (GParamFlags) (G_PARAM_READWRITE |
      GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  object_class->finalize = gst_decklink2_sink_finalize;
  object_class->set_property = gst_decklink2_sink_set_property;
  object_class->get_property = gst_decklink2_sink_get_property;

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

  g_object_class_install_property (object_class, PROP_VIDEO_FORMAT,
      g_param_spec_enum ("video-format", "Video format",
          "Video format type to use for playback",
          GST_TYPE_DECKLINK2_VIDEO_FORMAT, DEFAULT_VIDEO_FORMAT, param_flags));

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

  g_object_class_install_property (object_class, PROP_KEYER_MODE,
      g_param_spec_enum ("keyer-mode", "Keyer mode",
          "Keyer mode to be enabled",
          GST_TYPE_DECKLINK2_KEYER_MODE, DEFAULT_KEYER_MODE, param_flags));

  g_object_class_install_property (object_class, PROP_KEYER_LEVEL,
      g_param_spec_int ("keyer-level", "Keyer level",
          "Keyer level", 0, 255, DEFAULT_KEYER_LEVEL, param_flags));

  g_object_class_install_property (object_class, PROP_CC_LINE,
      g_param_spec_int ("cc-line", "CC Line",
          "Line number to use for inserting closed captions (0 = disabled)",
          0, 22, DEFAULT_CC_LINE, param_flags));

  g_object_class_install_property (object_class, PROP_AFD_BAR_LINE,
      g_param_spec_int ("afd-bar-line", "AFD/Bar Line",
          "Line number to use for inserting AFD/Bar data (0 = disabled)",
          0, 10000, DEFAULT_AFD_BAR_LINE, param_flags));

  g_object_class_install_property (object_class, PROP_MAPPING_FORMAT,
      g_param_spec_enum ("mapping-format", "3G-SDI Mapping Format",
          "3G-SDI Mapping Format (Level A/B)",
          GST_TYPE_DECKLINK2_MAPPING_FORMAT, DEFAULT_MAPPING_FORMAT,
          param_flags));

  g_object_class_install_property (object_class, PROP_N_PREROLL_FRAMES,
      g_param_spec_int ("n-preroll-frames", "Number of preroll frames",
          "How many frames to preroll before starting scheduled playback",
          0, 16, DEFAULT_N_PREROLL_FRAMES, param_flags));

  g_object_class_install_property (object_class, PROP_MIN_BUFFERED_FRAMES,
      g_param_spec_int ("min-buffered-frames", "Min number of buffered frames",
          "Min number of frames to buffer before duplicating",
          0, 16, DEFAULT_MIN_BUFFERED_FRAMES, param_flags));

  g_object_class_install_property (object_class, PROP_MAX_BUFFERED_FRAMES,
      g_param_spec_int ("max-buffered-frames", "Max number of buffered frames",
          "Max number of frames to buffer before dropping",
          0, 16, DEFAULT_MAX_BUFFERED_FRAMES, param_flags));

  g_object_class_install_property (object_class, PROP_AUTO_RESTART,
      g_param_spec_boolean ("auto-restart", "Auto Restart",
          "Restart streaming when frame is being dropped by hardware",
          DEFAULT_AUTO_RESTART,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class, PROP_OUTPUT_STATS,
      g_param_spec_boxed ("output-stats", "Output Statistics",
          "Output Statistics", GST_TYPE_STRUCTURE,
          (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

  gst_decklink2_sink_signals[SIGNAL_RESTART] =
      g_signal_new_class_handler ("restart", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_CALLBACK (gst_decklink2_sink_restart),
      NULL, NULL, NULL, G_TYPE_NONE, 0);

  GstCaps *templ_caps = gst_decklink2_get_default_template_caps ();
  templ_caps = gst_caps_make_writable (templ_caps);

  GValue ch_list = G_VALUE_INIT;
  gint ch[] = { 0, 2, 8, 16 };
  g_value_init (&ch_list, GST_TYPE_LIST);
  for (guint i = 0; i < G_N_ELEMENTS (ch); i++) {
    GValue ch_val = G_VALUE_INIT;
    g_value_init (&ch_val, G_TYPE_INT);
    g_value_set_int (&ch_val, ch[i]);
    gst_value_list_append_and_take_value (&ch_list, &ch_val);
  }

  gst_caps_set_value (templ_caps, "audio-channels", &ch_list);
  g_value_unset (&ch_list);

  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref (templ_caps);

  gst_element_class_set_static_metadata (element_class,
      "Decklink2 Sink", "Video/Audio/Sink/Hardware", "Decklink2 Sink",
      "Seungha Yang <seungha@centricular.com>");

  basesink_class->query = GST_DEBUG_FUNCPTR (gst_decklink2_sink_query);
  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_decklink2_sink_get_caps);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_decklink2_sink_set_caps);
  basesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_decklink2_sink_propose_allocation);
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_decklink2_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_decklink2_sink_stop);
  basesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_decklink2_sink_unlock_stop);
  basesink_class->prepare = GST_DEBUG_FUNCPTR (gst_decklink2_sink_prepare);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_decklink2_sink_render);

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_sink_debug, "decklink2sink",
      0, "decklink2sink");

  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_MODE, (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_VIDEO_FORMAT,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_PROFILE_ID,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_TIMECODE_FORMAT,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_KEYER_MODE,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_MAPPING_FORMAT,
      (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_DECKLINK2_MAPPING_FORMAT,
      (GstPluginAPIFlags) 0);
}

static void
gst_decklink2_sink_init (GstDeckLink2Sink * self)
{
  self->display_mode = DEFAULT_MODE;
  self->device_number = DEFAULT_DEVICE_NUMBER;
  self->persistent_id = DEFAULT_PERSISTENT_ID;
  self->video_format = DEFAULT_VIDEO_FORMAT;
  self->profile_id = DEFAULT_PROFILE_ID;
  self->timecode_format = DEFAULT_TIMECODE_FORMAT;
  self->keyer_mode = DEFAULT_KEYER_MODE;
  self->keyer_level = DEFAULT_KEYER_LEVEL;
  self->caption_line = DEFAULT_CC_LINE;
  self->afd_bar_line = DEFAULT_AFD_BAR_LINE;
  self->mapping_format = DEFAULT_MAPPING_FORMAT;
  self->n_preroll_frames = DEFAULT_N_PREROLL_FRAMES;
  self->min_buffered_frames = DEFAULT_MIN_BUFFERED_FRAMES;
  self->max_buffered_frames = DEFAULT_MAX_BUFFERED_FRAMES;
  self->auto_restart = DEFAULT_AUTO_RESTART;

  self->priv = new GstDeckLink2SinkPrivate ();
}

static void
gst_decklink2_sink_finalize (GObject * object)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (object);

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_decklink2_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (object);
  GstDeckLink2SinkPrivate *priv = self->priv;
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
    case PROP_VIDEO_FORMAT:
      self->video_format = (BMDPixelFormat) g_value_get_enum (value);
      break;
    case PROP_PROFILE_ID:
      self->profile_id = (BMDProfileID) g_value_get_enum (value);
      break;
    case PROP_TIMECODE_FORMAT:
      self->timecode_format = (BMDTimecodeFormat) g_value_get_enum (value);
      break;
    case PROP_KEYER_MODE:
      self->keyer_mode = (GstDeckLink2KeyerMode) g_value_get_enum (value);
      break;
    case PROP_KEYER_LEVEL:
      self->keyer_level = g_value_get_int (value);
      break;
    case PROP_CC_LINE:
      self->caption_line = g_value_get_int (value);
      break;
    case PROP_AFD_BAR_LINE:
      self->afd_bar_line = g_value_get_int (value);
      break;
    case PROP_MAPPING_FORMAT:
      self->mapping_format =
          (GstDeckLink2MappingFormat) g_value_get_enum (value);
      break;
    case PROP_N_PREROLL_FRAMES:
      self->n_preroll_frames = g_value_get_int (value);
      break;
    case PROP_MIN_BUFFERED_FRAMES:
      self->min_buffered_frames = g_value_get_int (value);
      break;
    case PROP_MAX_BUFFERED_FRAMES:
      self->max_buffered_frames = g_value_get_int (value);
      break;
    case PROP_AUTO_RESTART:
      self->auto_restart = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_decklink2_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (object);
  GstDeckLink2SinkPrivate *priv = self->priv;
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
    case PROP_VIDEO_FORMAT:
      g_value_set_enum (value, self->video_format);
      break;
    case PROP_PROFILE_ID:
      g_value_set_enum (value, self->profile_id);
      break;
    case PROP_TIMECODE_FORMAT:
      g_value_set_enum (value, self->timecode_format);
      break;
    case PROP_KEYER_MODE:
      g_value_set_enum (value, self->keyer_mode);
      break;
    case PROP_KEYER_LEVEL:
      g_value_set_int (value, self->keyer_level);
      break;
    case PROP_CC_LINE:
      g_value_set_int (value, self->caption_line);
      break;
    case PROP_AFD_BAR_LINE:
      g_value_set_int (value, self->afd_bar_line);
      break;
    case PROP_MAPPING_FORMAT:
      g_value_set_enum (value, self->mapping_format);
      break;
    case PROP_N_PREROLL_FRAMES:
      g_value_set_int (value, self->n_preroll_frames);
      break;
    case PROP_MIN_BUFFERED_FRAMES:
      g_value_set_int (value, self->min_buffered_frames);
      break;
    case PROP_MAX_BUFFERED_FRAMES:
      g_value_set_int (value, self->max_buffered_frames);
      break;
    case PROP_AUTO_RESTART:
      g_value_set_boolean (value, self->auto_restart);
      break;
    case PROP_OUTPUT_STATS:
    {
      GstStructure *s;
      GstDecklink2OutputStats *stats = &self->stats;

      s = gst_structure_new ("output-stats",
          "buffered-video", G_TYPE_UINT, stats->buffered_video,
          "buffered-audio", G_TYPE_UINT, stats->buffered_audio,
          "buffered-video-time", G_TYPE_UINT64, stats->buffered_video_time,
          "buffered-audio-time", G_TYPE_UINT64, stats->buffered_audio_time,
          "video-running-time", G_TYPE_UINT64, stats->video_running_time,
          "audio-running-time", G_TYPE_UINT64, stats->audio_running_time,
          "hardware-time", G_TYPE_UINT64, stats->hw_time,
          "scheduled-video-frames", G_TYPE_UINT64,
          stats->scheduled_video_frames, "scheduled-audio-samples",
          G_TYPE_UINT64, stats->scheduled_audio_samples, "dropped-frames",
          G_TYPE_UINT64, stats->drop_count, "dropped-samples", G_TYPE_UINT64,
          stats->dropped_sample_count, "late-count", G_TYPE_UINT64,
          stats->late_count, "overrun-count", G_TYPE_UINT64,
          stats->overrun_count, "underrun-count", G_TYPE_UINT64,
          stats->underrun_count, "duplicated-frames", G_TYPE_UINT64,
          stats->duplicate_count, "silent-samples", G_TYPE_UINT64,
          stats->silent_sample_count, NULL);
      g_value_take_boxed (value, s);

      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_decklink2_sink_query (GstBaseSink * sink, GstQuery * query)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps, *allowed;
      gboolean can_intercept;

      gst_query_parse_accept_caps (query, &caps);
      allowed = gst_decklink2_sink_get_caps (sink, NULL);
      can_intercept = gst_caps_can_intersect (caps, allowed);
      GST_DEBUG_OBJECT (self, "Checking if requested caps %" GST_PTR_FORMAT
          " are intersectable of pad caps %" GST_PTR_FORMAT " result %d", caps,
          allowed, can_intercept);
      gst_caps_unref (allowed);
      gst_query_set_accept_caps_result (query, can_intercept);
      return TRUE;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->query (sink, query);
}

static GstCaps *
gst_decklink2_sink_get_caps (GstBaseSink * sink, GstCaps * filter)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);
  GstDeckLink2SinkPrivate *priv = self->priv;
  GstCaps *caps;
  GstCaps *ret;
  std::lock_guard < std::mutex > lk (priv->lock);

  if (!self->output) {
    GST_DEBUG_OBJECT (self, "Output is not configured yet");
    caps = gst_pad_get_pad_template_caps (GST_BASE_SINK_PAD (self));
  } else {
    caps = gst_decklink2_output_get_caps (self->output, self->display_mode,
        self->video_format);
  }

  if (!caps) {
    GST_WARNING_OBJECT (self, "Couldn't get caps");
    caps = gst_caps_new_empty ();
  } else if (self->output) {
    guint max_ch;
    GValue ch_list = G_VALUE_INIT;
    GValue ch_val = G_VALUE_INIT;

    max_ch = gst_decklink2_output_get_max_audio_channels (self->output);

    caps = gst_caps_make_writable (caps);

    g_value_init (&ch_list, GST_TYPE_LIST);

    g_value_init (&ch_val, G_TYPE_INT);
    g_value_set_int (&ch_val, 0);
    gst_value_list_append_and_take_value (&ch_list, &ch_val);

    g_value_init (&ch_val, G_TYPE_INT);
    g_value_set_int (&ch_val, 2);
    gst_value_list_append_and_take_value (&ch_list, &ch_val);

    if (max_ch >= 8) {
      g_value_init (&ch_val, G_TYPE_INT);
      g_value_set_int (&ch_val, 8);
      gst_value_list_append_and_take_value (&ch_list, &ch_val);
    }

    if (max_ch >= 16) {
      g_value_init (&ch_val, G_TYPE_INT);
      g_value_set_int (&ch_val, 16);
      gst_value_list_append_and_take_value (&ch_list, &ch_val);
    }

    gst_caps_set_value (caps, "audio-channels", &ch_list);
    g_value_unset (&ch_list);
  }

  if (filter) {
    ret = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
  } else {
    ret = caps;
  }

  GST_LOG_OBJECT (self, "Returning caps %" GST_PTR_FORMAT, ret);

  return ret;
}

static gboolean
gst_decklink2_sink_set_caps (GstBaseSink * sink, GstCaps * caps)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);
  HRESULT hr;
  GstVideoInfo info;
  GstDeckLink2DisplayMode mode;
  GstStructure *config;
  GstAllocationParams params = { (GstMemoryFlags) 0, 15, 0, 0 };
  GstStructure *s;
  GstAudioFormat audio_format = GST_AUDIO_FORMAT_UNKNOWN;
  const gchar *audio_format_str;
  gint audio_channels = 0;
  BMDAudioSampleType audio_sample_type = bmdAudioSampleType16bitInteger;

  GST_DEBUG_OBJECT (self, "Set caps %" GST_PTR_FORMAT, caps);

  if (!self->output) {
    GST_ERROR_OBJECT (self, "output has not been configured yet");
    return FALSE;
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_ERROR_OBJECT (self, "Invalid caps");
    return FALSE;
  }

  if (!gst_decklink2_output_get_display_mode (self->output, &info, &mode)) {
    GST_ERROR_OBJECT (self, "Couldn't get display mode");
    return FALSE;
  }

  self->video_info = info;

  s = gst_caps_get_structure (caps, 0);
  audio_format_str = gst_structure_get_string (s, "audio-format");
  if (audio_format_str)
    audio_format = gst_audio_format_from_string (audio_format_str);
  gst_structure_get_int (s, "audio-channels", &audio_channels);

  if (audio_format == GST_AUDIO_FORMAT_S16LE) {
    audio_sample_type = bmdAudioSampleType16bitInteger;
  } else if (audio_format == GST_AUDIO_FORMAT_S32LE) {
    audio_sample_type = bmdAudioSampleType32bitInteger;
  } else {
    audio_channels = 0;
  }

  if (self->configured) {
    if (self->selected_mode.mode == mode.mode &&
        self->audio_sample_type == audio_sample_type &&
        self->audio_channels == audio_channels) {
      return TRUE;
    }

    GST_DEBUG_OBJECT (self, "Configuration changed");
    gst_decklink2_output_stop (self->output);
    self->configured = FALSE;
  }

  self->selected_mode = mode;
  self->audio_sample_type = audio_sample_type;
  self->audio_channels = audio_channels;

  /* The timecode_format itself is used when we embed the actual timecode data
   * into the frame. Now we only need to know which of the two standards the
   * timecode format will adhere to: VITC or RP188, and send the appropriate
   * flag to EnableVideoOutput. The exact format is specified later.
   *
   * Note that this flag will have no effect in practice if the video stream
   * does not contain timecode metadata.
   */
  if (self->timecode_format == bmdTimecodeVITC ||
      self->timecode_format == bmdTimecodeVITCField2) {
    self->output_flags = bmdVideoOutputVITC;
  } else {
    self->output_flags = bmdVideoOutputRP188;
  }

  if (self->caption_line > 0 || self->afd_bar_line > 0) {
    self->output_flags = (BMDVideoOutputFlags)
        (self->output_flags | bmdVideoOutputVANC);
  }

  GST_DEBUG_OBJECT (self, "Configuring output, mode %" GST_FOURCC_FORMAT
      ", audio-sample-type %d, audio-channles %d",
      GST_DECKLINK2_FOURCC_ARGS (self->selected_mode.mode),
      self->audio_sample_type, self->audio_channels);

  hr = gst_decklink2_output_configure (self->output, self->n_preroll_frames,
      self->min_buffered_frames, self->max_buffered_frames,
      &self->selected_mode, self->output_flags, self->profile_id,
      self->keyer_mode, (guint8) self->keyer_level, self->mapping_format,
      self->audio_sample_type, self->audio_channels);
  if (hr != S_OK) {
    GST_ERROR_OBJECT (self, "Couldn't configure output");
    return FALSE;
  }

  if (self->fallback_pool) {
    gst_buffer_pool_set_active (self->fallback_pool, FALSE);
    gst_object_unref (self->fallback_pool);
  }

  self->fallback_pool = gst_video_buffer_pool_new ();
  config = gst_buffer_pool_get_config (self->fallback_pool);
  gst_buffer_pool_config_set_params (config, caps, info.size, 0, 0);
  gst_buffer_pool_config_set_allocator (config, NULL, &params);

  if (!gst_buffer_pool_set_config (self->fallback_pool, config)) {
    GST_ERROR_OBJECT (self, "Couldn't set pool config");
    goto error;
  }

  if (!gst_buffer_pool_set_active (self->fallback_pool, TRUE)) {
    GST_ERROR_OBJECT (self, "Couldn't set active state to pool");
    goto error;
  }

  self->configured = TRUE;
  {
    std::lock_guard < std::mutex > lk (self->priv->lock);
    self->schedule_restart = FALSE;
  }

  return TRUE;

error:
  gst_clear_object (&self->fallback_pool);

  return FALSE;
}

static gboolean
gst_decklink2_sink_propose_allocation (GstBaseSink * sink, GstQuery * query)
{
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);

  if (!caps)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  size = GST_VIDEO_INFO_SIZE (&info);
  if (gst_query_get_n_allocation_pools (query) == 0) {
    GstStructure *structure;
    GstAllocator *allocator = NULL;
    GstAllocationParams params = { (GstMemoryFlags) 0, 15, 0, 0 };

    if (gst_query_get_n_allocation_params (query) > 0)
      gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    else
      gst_query_add_allocation_param (query, allocator, &params);

    pool = gst_video_buffer_pool_new ();

    structure = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (structure, caps, size, 0, 0);
    gst_buffer_pool_config_set_allocator (structure, allocator, &params);

    if (allocator)
      gst_object_unref (allocator);

    if (!gst_buffer_pool_set_config (pool, structure)) {
      GST_ERROR_OBJECT (sink, "Couldn't set pool config");
      gst_object_unref (pool);
      return FALSE;
    }

    gst_query_add_allocation_pool (query, pool, size, 0, 0);
    gst_object_unref (pool);
    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  }

  return TRUE;
}

static gboolean
gst_decklink2_sink_start (GstBaseSink * sink)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);
  GstDeckLink2SinkPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  GST_DEBUG_OBJECT (self, "Start");

  memset (&self->stats, 0, sizeof (GstDecklink2OutputStats));

  self->output = gst_decklink2_acquire_output (self->device_number,
      self->persistent_id);

  if (!self->output) {
    GST_ERROR_OBJECT (self, "Couldn't acquire output object");
    return FALSE;
  }

  if (self->n_preroll_frames < self->min_buffered_frames ||
      self->n_preroll_frames > self->max_buffered_frames ||
      self->max_buffered_frames < self->min_buffered_frames) {
    GST_WARNING_OBJECT (self, "Invalid buffering configuration");
    self->n_preroll_frames = DEFAULT_N_PREROLL_FRAMES;
    self->min_buffered_frames = DEFAULT_MIN_BUFFERED_FRAMES;
    self->max_buffered_frames = DEFAULT_MAX_BUFFERED_FRAMES;
  }

  memset (&self->selected_mode, 0, sizeof (GstDeckLink2DisplayMode));
  self->audio_sample_type = bmdAudioSampleType16bitInteger;
  self->audio_channels = 0;
  self->configured = FALSE;

  return TRUE;
}

static gboolean
gst_decklink2_sink_stop (GstBaseSink * sink)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);
  GstDeckLink2SinkPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  GST_DECKLINK2_CLEAR_COM (self->prepared_frame);

  if (self->output) {
    gst_decklink2_output_stop (self->output);
    gst_decklink2_release_output (self->output);
    self->output = NULL;
  }

  if (self->fallback_pool) {
    gst_buffer_pool_set_active (self->fallback_pool, FALSE);
    gst_clear_object (&self->fallback_pool);
  }

  self->schedule_restart = FALSE;

  return TRUE;
}

static gboolean
gst_decklink2_sink_unlock_stop (GstBaseSink * sink)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);

  GST_DECKLINK2_CLEAR_COM (self->prepared_frame);

  return TRUE;
}

static gboolean
buffer_is_pbo_memory (GstBuffer * buffer)
{
  GstMemory *mem;

  mem = gst_buffer_peek_memory (buffer, 0);
  if (mem->allocator
      && g_strcmp0 (mem->allocator->mem_type, "GLMemoryPBO") == 0)
    return TRUE;
  return FALSE;
}

static IDeckLinkVideoFrame *
gst_decklink2_sink_upload_frame (GstDeckLink2Sink * self, GstBuffer * buffer)
{
  GstBuffer *uploaded_buffer = buffer;

  if (buffer_is_pbo_memory (buffer)) {
    GstVideoFrame other_frame;
    GstVideoFrame vframe;
    GstBuffer *fallback;

    if (!gst_video_frame_map (&vframe, &self->video_info, buffer, GST_MAP_READ)) {
      GST_ERROR_OBJECT (self, "Failed to map video frame");
      return NULL;
    }

    GstFlowReturn ret = gst_buffer_pool_acquire_buffer (self->fallback_pool,
        &fallback, NULL);
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Couldn't acquire fallback buffer");
      gst_video_frame_unmap (&vframe);
      return NULL;
    }

    if (!gst_video_frame_map (&other_frame,
            &self->video_info, fallback, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (self, "Couldn't map fallback buffer");
      gst_video_frame_unmap (&vframe);
      gst_buffer_unref (fallback);
      return NULL;
    }

    if (!gst_video_frame_copy (&other_frame, &vframe)) {
      GST_ERROR_OBJECT (self, "Couldn't copy to fallback buffer");
      gst_video_frame_unmap (&vframe);
      gst_video_frame_unmap (&other_frame);
      gst_buffer_unref (fallback);
      return NULL;
    }

    gst_video_frame_unmap (&vframe);
    gst_video_frame_unmap (&other_frame);

    gst_buffer_copy_into (fallback, buffer, GST_BUFFER_COPY_META, 0, -1);
    uploaded_buffer = fallback;
  }

  IDeckLinkVideoFrame *frame =
      gst_decklink2_output_upload (self->output, &self->video_info,
      uploaded_buffer, self->caption_line, self->afd_bar_line);
  /* frame will hold buffer */
  if (uploaded_buffer != buffer)
    gst_buffer_unref (uploaded_buffer);

  return frame;
}

static GstFlowReturn
gst_decklink2_sink_prepare (GstBaseSink * sink, GstBuffer * buffer)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);

  GST_DECKLINK2_CLEAR_COM (self->prepared_frame);

  self->prepared_frame = gst_decklink2_sink_upload_frame (self, buffer);
  if (!self->prepared_frame) {
    GST_ERROR_OBJECT (self, "Couldn't upload frame");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_decklink2_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstDeckLink2Sink *self = GST_DECKLINK2_SINK (sink);
  GstDeckLink2SinkPrivate *priv = self->priv;
  HRESULT hr;
  GstBuffer *audio_buf = NULL;
  GstMapInfo info;
  guint8 *audio_data = NULL;
  gsize audio_data_size = 0;
  GstDecklink2OutputStats stats = { 0, };

  if (!self->prepared_frame) {
    GST_ERROR_OBJECT (self, "No prepared frame");
    return GST_FLOW_ERROR;
  }

  if (self->audio_channels > 0) {
    GstDeckLink2AudioMeta *meta = gst_buffer_get_decklink2_audio_meta (buffer);
    if (meta)
      audio_buf = gst_sample_get_buffer (meta->sample);

    if (audio_buf) {
      if (!gst_buffer_map (audio_buf, &info, GST_MAP_READ)) {
        GST_ERROR_OBJECT (self, "Couldn't map audio buffer");
        return GST_FLOW_ERROR;
      }

      audio_data = info.data;
      audio_data_size = info.size;
    } else {
      GST_DEBUG_OBJECT (self, "Received buffer without audio meta");
    }
  }

  {
    std::lock_guard < std::mutex > lk (priv->lock);

    if (self->schedule_restart) {
      GST_DEBUG_OBJECT (self, "Restarting sink as scheduled");
      self->schedule_restart = FALSE;

      hr = gst_decklink2_output_configure (self->output, self->n_preroll_frames,
          self->min_buffered_frames, self->max_buffered_frames,
          &self->selected_mode, self->output_flags, self->profile_id,
          self->keyer_mode, (guint8) self->keyer_level, self->mapping_format,
          self->audio_sample_type, self->audio_channels);

      if (hr != S_OK) {
        GST_ERROR_OBJECT (self, "Couldn't configure output");
        return GST_FLOW_OK;
      }
    }
  }

  GST_LOG_OBJECT (self, "schedule frame %p with audio buffer size %"
      G_GSIZE_FORMAT, self->prepared_frame, audio_data_size);

  hr = gst_decklink2_output_schedule_stream (self->output,
      self->prepared_frame, audio_data, audio_data_size, &stats);

  if (audio_buf)
    gst_buffer_unmap (audio_buf, &info);

  if (hr != S_OK) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("Failed to schedule frame: 0x%x", (guint) hr));
    return GST_FLOW_ERROR;
  }

  if (self->auto_restart && (stats.drop_count + stats.late_count >
          (guint) self->n_preroll_frames)) {
    GST_WARNING_OBJECT (self, "Restart output, drop count: %" G_GUINT64_FORMAT
        ", late cout: %" G_GUINT64_FORMAT ", underrun count: %"
        G_GUINT64_FORMAT, stats.drop_count, stats.late_count,
        stats.underrun_count);

    hr = gst_decklink2_output_configure (self->output, self->n_preroll_frames,
        self->min_buffered_frames, self->max_buffered_frames,
        &self->selected_mode, self->output_flags, self->profile_id,
        self->keyer_mode, (guint8) self->keyer_level, self->mapping_format,
        self->audio_sample_type, self->audio_channels);

    if (hr != S_OK) {
      GST_ERROR_OBJECT (self, "Couldn't configure output");
      return GST_FLOW_OK;
    }
  }

  std::lock_guard < std::mutex > lk (priv->lock);
  self->stats = stats;

  return GST_FLOW_OK;
}

static void
gst_decklink2_sink_restart (GstDeckLink2Sink * self)
{
  GstDeckLink2SinkPrivate *priv = self->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  GST_DEBUG_OBJECT (self, "Schedule restart");
  self->schedule_restart = TRUE;
}
