/* GStreamer
 * Copyright (C) 2011 David Schleef <ds@entropywave.com>
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2021 Mathieu Duponchelle <mathieu@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

/**
 * SECTION:element-decklinksink
 * @short_description: Outputs Audio / Video to a BlackMagic DeckLink Device
 *
 * Playout Audio / Video to a BlackMagic DeckLink Device.
 *
 * ## Sample pipeline
 *
 * ``` shell
 * gst-launch-1.0 -v avcombiner latency=500000000 name=comb ! queue ! decklinksink mode=1080p25 \
 *   videotestsrc is-live=true ! videorate ! video/x-raw, framerate=25/1, width=1920, height=1080 ! \
 *     queue ! timeoverlay ! comb.video \
 *   audiotestsrc is-live=true ! audio/x-raw, format=S32LE, channels=2, rate=48000 ! queue ! \
 *     audiobuffersplit output-buffer-duration=1/25 ! queue ! comb.audio
 * ```
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdecklinksink.h"
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_decklink_sink_debug);
#define GST_CAT_DEFAULT gst_decklink_sink_debug

static GstFlowReturn schedule_buffer (GstDecklinkSink *self, GstBuffer *buffer);

class OutputCallback:public IDeckLinkVideoOutputCallback
{
public:
  OutputCallback (GstDecklinkSink * sink)
  :IDeckLinkVideoOutputCallback (), m_refcount (1)
  {
    m_sink = GST_DECKLINK_SINK_CAST (gst_object_ref (sink));
    g_mutex_init (&m_mutex);
  }

  virtual HRESULT WINAPI QueryInterface (REFIID, LPVOID *)
  {
    return E_NOINTERFACE;
  }

  virtual ULONG WINAPI AddRef (void)
  {
    ULONG ret;

    g_mutex_lock (&m_mutex);
    m_refcount++;
    ret = m_refcount;
    g_mutex_unlock (&m_mutex);

    return ret;
  }

  virtual ULONG WINAPI Release (void)
  {
    ULONG ret;

    g_mutex_lock (&m_mutex);
    m_refcount--;
    ret = m_refcount;
    g_mutex_unlock (&m_mutex);

    if (ret == 0) {
      delete this;
    }

    return ret;
  }

  virtual HRESULT WINAPI ScheduledFrameCompleted (IDeckLinkVideoFrame *
      completedFrame, BMDOutputFrameCompletionResult result)
  {
    uint32_t bufferedFrameCount;
    bool active = false;

    switch (result) {
      case bmdOutputFrameCompleted:
        GST_LOG_OBJECT (m_sink, "Completed frame %p", completedFrame);
        break;
      case bmdOutputFrameDisplayedLate:
        GST_WARNING_OBJECT (m_sink, "Late Frame %p", completedFrame);
        break;
      case bmdOutputFrameDropped:
        GST_ERROR_OBJECT (m_sink, "Dropped Frame %p", completedFrame);
        break;
      case bmdOutputFrameFlushed:
        GST_DEBUG_OBJECT (m_sink, "Flushed Frame %p", completedFrame);
        break;
      default:
        GST_ERROR_OBJECT (m_sink, "Unknown Frame %p: %d", completedFrame,
            (gint) result);
        break;
    }

    g_mutex_lock(&m_sink->schedule_lock);

    m_sink->output->output->IsScheduledPlaybackRunning (&active);

    if (m_sink->output->output->
        GetBufferedVideoFrameCount (&bufferedFrameCount) == S_OK) {
      GST_TRACE_OBJECT (m_sink, "%u video frames buffered", bufferedFrameCount);

      if (active && bufferedFrameCount <= (uint32_t) m_sink->min_buffered_frames) {
        GST_WARNING_OBJECT (m_sink, "Number of buffered frames dipped below threshold, duplicating!");
        schedule_buffer (m_sink, m_sink->last_buffer);
      }
    }

    if (m_sink->output->output->
        GetBufferedAudioSampleFrameCount (&bufferedFrameCount) == S_OK)
      GST_TRACE_OBJECT (m_sink, "%u audio frames buffered", bufferedFrameCount);

    g_mutex_unlock(&m_sink->schedule_lock);

    return S_OK;
  }

  virtual HRESULT WINAPI ScheduledPlaybackHasStopped (void)
  {
    GST_INFO_OBJECT (m_sink, "Scheduled playback stopped");

    return S_OK;
  }

  virtual ~ OutputCallback () {
    GST_ERROR ("Destroyed");
    gst_object_unref (m_sink);
    g_mutex_clear (&m_mutex);
  }

private:
  GstDecklinkSink * m_sink;
  GMutex m_mutex;
  gint m_refcount;
};

enum
{
  PROP_0,
  PROP_MODE,
  PROP_DEVICE_NUMBER,
  PROP_VIDEO_FORMAT,
  PROP_TIMECODE_FORMAT,
  PROP_KEYER_MODE,
  PROP_KEYER_LEVEL,
  PROP_HW_SERIAL_NUMBER,
  PROP_CC_LINE,
  PROP_AFD_BAR_LINE,
  PROP_N_PREROLL_FRAMES,
  PROP_MIN_BUFFERED_FRAMES,
  PROP_MAX_BUFFERED_FRAMES,
};

static void gst_decklink_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_decklink_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_decklink_sink_finalize (GObject * object);

static GstStateChangeReturn
gst_decklink_sink_change_state (GstElement * element,
    GstStateChange transition);

static GstCaps *gst_decklink_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);
static gboolean gst_decklink_sink_set_caps (GstBaseSink * bsink,
    GstCaps * caps);
static GstFlowReturn gst_decklink_sink_render (GstBaseSink * bsink,
    GstBuffer * buffer);
static gboolean gst_decklink_sink_open (GstBaseSink * bsink);
static gboolean gst_decklink_sink_close (GstBaseSink * bsink);
static gboolean gst_decklink_sink_propose_allocation (GstBaseSink * bsink,
    GstQuery * query);
static gboolean gst_decklink_sink_event (GstBaseSink * bsink, GstEvent * event);

#define parent_class gst_decklink_sink_parent_class
G_DEFINE_TYPE (GstDecklinkSink, gst_decklink_sink, GST_TYPE_BASE_SINK);
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (decklinksink, "decklinksink", GST_RANK_NONE,
                                       GST_TYPE_DECKLINK_SINK, decklink_element_init (plugin));


static gboolean
reset_framerate (GstCapsFeatures * features, GstStructure * structure,
    gpointer user_data)
{
  gst_structure_set (structure, "framerate", GST_TYPE_FRACTION_RANGE, 0, 1,
      G_MAXINT, 1, NULL);

  return TRUE;
}

static gboolean
set_audio_channels (GstCapsFeatures * features, GstStructure * structure,
    gpointer user_data)
{
  GValue v = G_VALUE_INIT;
  GValue v2 = G_VALUE_INIT;

  g_value_init (&v, GST_TYPE_LIST);

  g_value_init (&v2, G_TYPE_INT);
  g_value_set_int (&v2, 2);
  gst_value_list_append_value (&v, &v2);
  g_value_unset (&v2);

  g_value_init (&v2, G_TYPE_INT);
  g_value_set_int (&v2, 8);
  gst_value_list_append_value (&v, &v2);
  g_value_unset (&v2);

  g_value_init (&v2, G_TYPE_INT);
  g_value_set_int (&v2, 16);
  gst_value_list_append_value (&v, &v2);
  g_value_unset (&v2);

  gst_structure_set_value (structure, "audio-channels", &v);

  g_value_unset (&v);

  return TRUE;
}

static void
gst_decklink_sink_class_init (GstDecklinkSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GstCaps *templ_caps;

  gobject_class->set_property = gst_decklink_sink_set_property;
  gobject_class->get_property = gst_decklink_sink_get_property;
  gobject_class->finalize = gst_decklink_sink_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_decklink_sink_change_state);

  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_decklink_sink_get_caps);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_decklink_sink_set_caps);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_decklink_sink_render);
  // FIXME: These are misnamed in basesink!
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_decklink_sink_open);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_decklink_sink_close);
  basesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_decklink_sink_propose_allocation);
  basesink_class->event = GST_DEBUG_FUNCPTR (gst_decklink_sink_event);

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Playback Mode",
          "Video Mode to use for playback",
          GST_TYPE_DECKLINK_MODE, GST_DECKLINK_MODE_NTSC,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_DEVICE_NUMBER,
      g_param_spec_int ("device-number", "Device number",
          "Output device instance to use", 0, G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_VIDEO_FORMAT,
      g_param_spec_enum ("video-format", "Video format",
          "Video format type to use for playback",
          GST_TYPE_DECKLINK_VIDEO_FORMAT, GST_DECKLINK_VIDEO_FORMAT_8BIT_YUV,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_TIMECODE_FORMAT,
      g_param_spec_enum ("timecode-format", "Timecode format",
          "Timecode format type to use for playback",
          GST_TYPE_DECKLINK_TIMECODE_FORMAT,
          GST_DECKLINK_TIMECODE_FORMAT_RP188ANY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_KEYER_MODE,
      g_param_spec_enum ("keyer-mode", "Keyer mode",
          "Keyer mode to be enabled",
          GST_TYPE_DECKLINK_KEYER_MODE,
          GST_DECKLINK_KEYER_MODE_OFF,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_KEYER_LEVEL,
      g_param_spec_int ("keyer-level", "Keyer level",
          "Keyer level", 0, 255, 255,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_HW_SERIAL_NUMBER,
      g_param_spec_string ("hw-serial-number", "Hardware serial number",
          "The serial number (hardware ID) of the Decklink card",
          NULL, (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CC_LINE,
      g_param_spec_int ("cc-line", "CC Line",
          "Line number to use for inserting closed captions (0 = disabled)", 0,
          22, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_AFD_BAR_LINE,
      g_param_spec_int ("afd-bar-line", "AFD/Bar Line",
          "Line number to use for inserting AFD/Bar data (0 = disabled)", 0,
          10000, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_N_PREROLL_FRAMES,
      g_param_spec_int ("n-preroll-frames", "Number of preroll frames",
          "How many frames to preroll before starting scheduled playback", 0,
          G_MAXINT, 7,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_MIN_BUFFERED_FRAMES,
      g_param_spec_int ("min-buffered-frames", "Min number of buffered frames",
          "Min number of frames to buffer before duplicating", 0,
          G_MAXINT, 3,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_MAX_BUFFERED_FRAMES,
      g_param_spec_int ("max-buffered-frames", "Max number of buffered frames",
          "Max number of frames to buffer before dropping", 0,
          G_MAXINT, 14,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  templ_caps = gst_decklink_mode_get_template_caps (FALSE);
  templ_caps = gst_caps_make_writable (templ_caps);
  /* For output we support any framerate and only really care about timestamps */
  gst_caps_map_in_place (templ_caps, reset_framerate, NULL);
  /* We expect the input to have audio meta */
  gst_caps_map_in_place (templ_caps, set_audio_channels, NULL);

  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref (templ_caps);

  gst_element_class_set_static_metadata (element_class, "Decklink Sink",
      "Audio/Video/Sink/Hardware", "Decklink Sink",
      "David Schleef <ds@entropywave.com>, "
      "Sebastian Dröge <sebastian@centricular.com>, "
      "Mathieu Duponchelle <mathieu@centricular.com>");

  GST_DEBUG_CATEGORY_INIT (gst_decklink_sink_debug, "decklinksink",
      0, "debug category for decklinksink element");
}

static void
gst_decklink_sink_init (GstDecklinkSink * self)
{
  g_mutex_init(&self->schedule_lock);
}

void
gst_decklink_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (object);

  switch (property_id) {
    case PROP_MODE:
      self->mode = (GstDecklinkModeEnum) g_value_get_enum (value);
      break;
    case PROP_DEVICE_NUMBER:
      self->device_number = g_value_get_int (value);
      break;
    case PROP_VIDEO_FORMAT:
      self->video_format = (GstDecklinkVideoFormat) g_value_get_enum (value);
      switch (self->video_format) {
        case GST_DECKLINK_VIDEO_FORMAT_AUTO:
        case GST_DECKLINK_VIDEO_FORMAT_8BIT_YUV:
        case GST_DECKLINK_VIDEO_FORMAT_10BIT_YUV:
        case GST_DECKLINK_VIDEO_FORMAT_8BIT_ARGB:
        case GST_DECKLINK_VIDEO_FORMAT_8BIT_BGRA:
          break;
        default:
          GST_ELEMENT_WARNING (GST_ELEMENT (self), CORE, NOT_IMPLEMENTED,
              ("Format %d not supported", self->video_format), (NULL));
          break;
      }
      break;
    case PROP_TIMECODE_FORMAT:
      self->timecode_format =
          gst_decklink_timecode_format_from_enum ((GstDecklinkTimecodeFormat)
          g_value_get_enum (value));
      break;
    case PROP_KEYER_MODE:
      self->keyer_mode =
          gst_decklink_keyer_mode_from_enum ((GstDecklinkKeyerMode)
          g_value_get_enum (value));
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
    case PROP_N_PREROLL_FRAMES:
      self->n_preroll_frames = g_value_get_int (value);
      break;
    case PROP_MIN_BUFFERED_FRAMES:
      self->min_buffered_frames = g_value_get_int (value);
      break;
    case PROP_MAX_BUFFERED_FRAMES:
      self->max_buffered_frames = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_decklink_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (object);

  switch (property_id) {
    case PROP_MODE:
      g_value_set_enum (value, self->mode);
      break;
    case PROP_DEVICE_NUMBER:
      g_value_set_int (value, self->device_number);
      break;
    case PROP_VIDEO_FORMAT:
      g_value_set_enum (value, self->video_format);
      break;
    case PROP_TIMECODE_FORMAT:
      g_value_set_enum (value,
          gst_decklink_timecode_format_to_enum (self->timecode_format));
      break;
    case PROP_KEYER_MODE:
      g_value_set_enum (value,
          gst_decklink_keyer_mode_to_enum (self->keyer_mode));
      break;
    case PROP_KEYER_LEVEL:
      g_value_set_int (value, self->keyer_level);
      break;
    case PROP_HW_SERIAL_NUMBER:
      if (self->output)
        g_value_set_string (value, self->output->hw_serial_number);
      else
        g_value_set_string (value, NULL);
      break;
    case PROP_CC_LINE:
      g_value_set_int (value, self->caption_line);
      break;
    case PROP_AFD_BAR_LINE:
      g_value_set_int (value, self->afd_bar_line);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_decklink_sink_finalize (GObject * object)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (object);

  g_mutex_clear(&self->schedule_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_decklink_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (bsink);
  const GstStructure *s;
  const GstDecklinkMode *mode;
  HRESULT ret;
  BMDVideoOutputFlags flags;
  GstVideoInfo info;
  gint audio_channels;

  GST_DEBUG_OBJECT (self, "Setting caps %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  s = gst_caps_get_structure (caps, 0);

  if (!gst_structure_get_int (s, "audio-channels", &audio_channels)) {
    GST_ERROR_OBJECT (self, "Expected audio-channels field in input caps");
    return FALSE;
  }

  if (self->output->video_enabled) {
    if (self->info.finfo->format == info.finfo->format &&
        self->info.width == info.width && self->info.height == info.height) {
      // FIXME: We should also consider the framerate as it is used
      // for mode selection below in auto mode
      GST_DEBUG_OBJECT (self, "Nothing relevant has changed");
      self->info = info;
      return TRUE;
    } else {
      GST_DEBUG_OBJECT (self, "Reconfiguration not supported at this point");
      return FALSE;
    }
  }

  if (self->output->audio_enabled) {
    if (self->audio_channels == audio_channels) {
      GST_DEBUG_OBJECT (self, "Nothing relevant has changed in the audio");
      return TRUE;
    } else {
      GST_DEBUG_OBJECT (self,
          "Audio reconfiguration not supported at this point");
      return FALSE;
    }
  }

  if (self->mode == GST_DECKLINK_MODE_AUTO) {
    BMDPixelFormat f;
    mode = gst_decklink_find_mode_and_format_for_caps (caps, &f);
    if (mode == NULL) {
      GST_WARNING_OBJECT (self,
          "Failed to find compatible mode for caps  %" GST_PTR_FORMAT, caps);
      return FALSE;
    }
    if (self->video_format != GST_DECKLINK_VIDEO_FORMAT_AUTO &&
        gst_decklink_pixel_format_from_type (self->video_format) != f) {
      GST_WARNING_OBJECT (self, "Failed to set pixel format to %d",
          self->video_format);
      return FALSE;
    }
  } else {
    /* We don't have to give the format in EnableVideoOutput. Therefore,
     * even if it's AUTO, we have it stored in self->info and set it in
     * gst_decklink_video_sink_render */
    mode = gst_decklink_get_mode (self->mode);
    g_assert (mode != NULL);
  };

  /* enable or disable keyer */
  if (self->output->keyer != NULL) {
    if (self->keyer_mode == bmdKeyerModeOff) {
      self->output->keyer->Disable ();
    } else if (self->keyer_mode == bmdKeyerModeInternal) {
      self->output->keyer->Enable (false);
      self->output->keyer->SetLevel (self->keyer_level);
    } else if (self->keyer_mode == bmdKeyerModeExternal) {
      self->output->keyer->Enable (true);
      self->output->keyer->SetLevel (self->keyer_level);
    } else {
      g_assert_not_reached ();
    }
  } else if (self->keyer_mode != bmdKeyerModeOff) {
    GST_WARNING_OBJECT (self, "Failed to set keyer to mode %d",
        self->keyer_mode);
  }

  /* The timecode_format itself is used when we embed the actual timecode data
   * into the frame. Now we only need to know which of the two standards the
   * timecode format will adhere to: VITC or RP188, and send the appropriate
   * flag to EnableVideoOutput. The exact format is specified later.
   *
   * Note that this flag will have no effect in practice if the video stream
   * does not contain timecode metadata.
   */
  if ((gint64) self->timecode_format ==
      (gint64) GST_DECKLINK_TIMECODE_FORMAT_VITC
      || (gint64) self->timecode_format ==
      (gint64) GST_DECKLINK_TIMECODE_FORMAT_VITCFIELD2)
    flags = bmdVideoOutputVITC;
  else
    flags = bmdVideoOutputRP188;

  if (self->caption_line > 0 || self->afd_bar_line > 0)
    flags = (BMDVideoOutputFlags) (flags | bmdVideoOutputVANC);

  ret = self->output->output->EnableVideoOutput (mode->mode, flags);
  if (ret != S_OK) {
    GST_WARNING_OBJECT (self, "Failed to enable video output: 0x%08lx",
        (unsigned long) ret);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Enabling Audio Output");

  ret = self->output->output->EnableAudioOutput (bmdAudioSampleRate48kHz,
      bmdAudioSampleType32bitInteger, audio_channels,
      bmdAudioOutputStreamContinuous);
  if (ret != S_OK) {
    GST_WARNING_OBJECT (self, "Failed to enable audio output 0x%08lx",
        (unsigned long) ret);
    return FALSE;
  }

  self->output->output->SetScheduledFrameCompletionCallback (new
      OutputCallback (self));

  self->info = info;
  self->audio_channels = audio_channels;
  self->output->mode = mode;
  self->output->video_enabled = TRUE;
  self->output->audio_enabled = TRUE;

  return TRUE;
}

static GstCaps *
gst_decklink_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (bsink);
  GstCaps *mode_caps, *caps;

  if (self->mode == GST_DECKLINK_MODE_AUTO
      && self->video_format == GST_DECKLINK_VIDEO_FORMAT_AUTO)
    mode_caps = gst_decklink_mode_get_template_caps (FALSE);
  else if (self->video_format == GST_DECKLINK_VIDEO_FORMAT_AUTO)
    mode_caps = gst_decklink_mode_get_caps_all_formats (self->mode, FALSE);
  else if (self->mode == GST_DECKLINK_MODE_AUTO)
    mode_caps =
        gst_decklink_pixel_format_get_caps (gst_decklink_pixel_format_from_type
        (self->video_format), FALSE);
  else
    mode_caps =
        gst_decklink_mode_get_caps (self->mode,
        gst_decklink_pixel_format_from_type (self->video_format), FALSE);
  mode_caps = gst_caps_make_writable (mode_caps);
  /* For output we support any framerate and only really care about timestamps */
  gst_caps_map_in_place (mode_caps, reset_framerate, NULL);

  /* We expect the input to have audio meta */
  gst_caps_map_in_place (mode_caps, set_audio_channels, NULL);

  if (filter) {
    caps =
        gst_caps_intersect_full (filter, mode_caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (mode_caps);
  } else {
    caps = mode_caps;
  }

  return caps;
}

/* Copied from ext/closedcaption/gstccconverter.c */
/* Converts raw CEA708 cc_data and an optional timecode into CDP */
static guint
convert_cea708_cc_data_cea708_cdp_internal (GstDecklinkSink * self,
    const guint8 * cc_data, guint cc_data_len, guint8 * cdp, guint cdp_len,
    const GstVideoTimeCodeMeta * tc_meta)
{
  GstByteWriter bw;
  guint8 flags, checksum;
  guint i, len;
  const GstDecklinkMode *mode = gst_decklink_get_mode (self->mode);

  gst_byte_writer_init_with_data (&bw, cdp, cdp_len, FALSE);
  gst_byte_writer_put_uint16_be_unchecked (&bw, 0x9669);
  /* Write a length of 0 for now */
  gst_byte_writer_put_uint8_unchecked (&bw, 0);
  if (mode->fps_n == 24000 && mode->fps_d == 1001) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x1f);
  } else if (mode->fps_n == 24 && mode->fps_d == 1) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x2f);
  } else if (mode->fps_n == 25 && mode->fps_d == 1) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x3f);
  } else if (mode->fps_n == 30000 && mode->fps_d == 1001) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x4f);
  } else if (mode->fps_n == 30 && mode->fps_d == 1) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x5f);
  } else if (mode->fps_n == 50 && mode->fps_d == 1) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x6f);
  } else if (mode->fps_n == 60000 && mode->fps_d == 1001) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x7f);
  } else if (mode->fps_n == 60 && mode->fps_d == 1) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x8f);
  } else {
    g_assert_not_reached ();
  }

  /* ccdata_present | caption_service_active */
  flags = 0x42;

  /* time_code_present */
  if (tc_meta)
    flags |= 0x80;

  /* reserved */
  flags |= 0x01;

  gst_byte_writer_put_uint8_unchecked (&bw, flags);

  gst_byte_writer_put_uint16_be_unchecked (&bw, self->cdp_hdr_sequence_cntr);

  if (tc_meta) {
    const GstVideoTimeCode *tc = &tc_meta->tc;
    guint8 u8;

    gst_byte_writer_put_uint8_unchecked (&bw, 0x71);
    /* reserved 11 - 2 bits */
    u8 = 0xc0;
    /* tens of hours - 2 bits */
    u8 |= ((tc->hours / 10) & 0x3) << 4;
    /* units of hours - 4 bits */
    u8 |= (tc->hours % 10) & 0xf;
    gst_byte_writer_put_uint8_unchecked (&bw, u8);

    /* reserved 1 - 1 bit */
    u8 = 0x80;
    /* tens of minutes - 3 bits */
    u8 |= ((tc->minutes / 10) & 0x7) << 4;
    /* units of minutes - 4 bits */
    u8 |= (tc->minutes % 10) & 0xf;
    gst_byte_writer_put_uint8_unchecked (&bw, u8);

    /* field flag - 1 bit */
    u8 = tc->field_count < 2 ? 0x00 : 0x80;
    /* tens of seconds - 3 bits */
    u8 |= ((tc->seconds / 10) & 0x7) << 4;
    /* units of seconds - 4 bits */
    u8 |= (tc->seconds % 10) & 0xf;
    gst_byte_writer_put_uint8_unchecked (&bw, u8);

    /* drop frame flag - 1 bit */
    u8 = (tc->config.flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) ? 0x80 :
        0x00;
    /* reserved0 - 1 bit */
    /* tens of frames - 2 bits */
    u8 |= ((tc->frames / 10) & 0x3) << 4;
    /* units of frames 4 bits */
    u8 |= (tc->frames % 10) & 0xf;
    gst_byte_writer_put_uint8_unchecked (&bw, u8);
  }

  gst_byte_writer_put_uint8_unchecked (&bw, 0x72);
  gst_byte_writer_put_uint8_unchecked (&bw, 0xe0 | cc_data_len / 3);
  gst_byte_writer_put_data_unchecked (&bw, cc_data, cc_data_len);

  gst_byte_writer_put_uint8_unchecked (&bw, 0x74);
  gst_byte_writer_put_uint16_be_unchecked (&bw, self->cdp_hdr_sequence_cntr);
  self->cdp_hdr_sequence_cntr++;
  /* We calculate the checksum afterwards */
  gst_byte_writer_put_uint8_unchecked (&bw, 0);

  len = gst_byte_writer_get_pos (&bw);
  gst_byte_writer_set_pos (&bw, 2);
  gst_byte_writer_put_uint8_unchecked (&bw, len);

  checksum = 0;
  for (i = 0; i < len; i++) {
    checksum += cdp[i];
  }
  checksum &= 0xff;
  checksum = 256 - checksum;
  cdp[len - 1] = checksum;

  return len;
}

static void
write_vbi (GstDecklinkSink * self, GstBuffer * buffer,
    BMDPixelFormat format, IDeckLinkMutableVideoFrame * frame,
    GstVideoTimeCodeMeta * tc_meta)
{
  IDeckLinkVideoFrameAncillary *vanc_frame = NULL;
  gpointer iter = NULL;
  GstVideoCaptionMeta *cc_meta;
  guint8 *vancdata;
  gboolean got_captions = FALSE;

  if (self->caption_line == 0 && self->afd_bar_line == 0)
    return;

  if (self->vbiencoder == NULL) {
    self->vbiencoder =
        gst_video_vbi_encoder_new (GST_VIDEO_FORMAT_v210, self->info.width);
    self->anc_vformat = GST_VIDEO_FORMAT_v210;
  }

  /* Put any closed captions into the configured line */
  while ((cc_meta =
          (GstVideoCaptionMeta *) gst_buffer_iterate_meta_filtered (buffer,
              &iter, GST_VIDEO_CAPTION_META_API_TYPE))) {
    switch (cc_meta->caption_type) {
      case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:{
        guint8 data[138];
        guint i, n;

        n = cc_meta->size / 2;
        if (cc_meta->size > 46) {
          GST_WARNING_OBJECT (self, "Too big raw CEA608 buffer");
          break;
        }

        /* This is the offset from line 9 for 525-line fields and from line
         * 5 for 625-line fields.
         *
         * The highest bit is set for field 1 but not for field 0, but we
         * have no way of knowning the field here
         */
        for (i = 0; i < n; i++) {
          data[3 * i] = 0x80 | (self->info.height ==
              525 ? self->caption_line - 9 : self->caption_line - 5);
          data[3 * i + 1] = cc_meta->data[2 * i];
          data[3 * i + 2] = cc_meta->data[2 * i + 1];
        }

        if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
                FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 & 0xff, data, 3))
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");

        got_captions = TRUE;

        break;
      }
      case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:{
        if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
                FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 & 0xff, cc_meta->data,
                cc_meta->size))
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");

        got_captions = TRUE;

        break;
      }
      case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:{
        guint8 data[256];
        guint n;

        n = cc_meta->size / 3;
        if (cc_meta->size > 46) {
          GST_WARNING_OBJECT (self, "Too big raw CEA708 buffer");
          break;
        }

        n = convert_cea708_cc_data_cea708_cdp_internal (self, cc_meta->data,
            cc_meta->size, data, sizeof (data), tc_meta);
        if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder, FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 & 0xff, data, n))
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");

        got_captions = TRUE;

        break;
      }
      case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:{
        if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
                FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 & 0xff, cc_meta->data,
                cc_meta->size))
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");

        got_captions = TRUE;

        break;
      }
      default:{
        GST_FIXME_OBJECT (self, "Caption type %d not supported",
            cc_meta->caption_type);
        break;
      }
    }
  }

  if ((got_captions || self->afd_bar_line != 0)
      && self->output->output->CreateAncillaryData (bmdFormat10BitYUV,
          &vanc_frame) == S_OK) {
    GstVideoAFDMeta *afd_meta = NULL, *afd_meta2 = NULL;
    GstVideoBarMeta *bar_meta = NULL, *bar_meta2 = NULL;
    GstMeta *meta;
    gpointer meta_iter;
    guint8 afd_bar_data[8] = { 0, };
    guint8 afd_bar_data2[8] = { 0, };
    guint8 afd = 0;
    gboolean is_letterbox = 0;
    guint16 bar1 = 0, bar2 = 0;
    guint i;

    // Get any reasonable AFD/Bar metas for both fields
    meta_iter = NULL;
    while ((meta =
            gst_buffer_iterate_meta_filtered (buffer, &meta_iter,
                GST_VIDEO_AFD_META_API_TYPE))) {
      GstVideoAFDMeta *tmp_meta = (GstVideoAFDMeta *) meta;

      if (tmp_meta->field == 0 || !afd_meta || (afd_meta && afd_meta->field != 0
              && tmp_meta->field == 0))
        afd_meta = tmp_meta;
      if (tmp_meta->field == 1 || !afd_meta2 || (afd_meta2
              && afd_meta->field != 1 && tmp_meta->field == 1))
        afd_meta2 = tmp_meta;
    }

    meta_iter = NULL;
    while ((meta =
            gst_buffer_iterate_meta_filtered (buffer, &meta_iter,
                GST_VIDEO_BAR_META_API_TYPE))) {
      GstVideoBarMeta *tmp_meta = (GstVideoBarMeta *) meta;

      if (tmp_meta->field == 0 || !bar_meta || (bar_meta && bar_meta->field != 0
              && tmp_meta->field == 0))
        bar_meta = tmp_meta;
      if (tmp_meta->field == 1 || !bar_meta2 || (bar_meta2
              && bar_meta->field != 1 && tmp_meta->field == 1))
        bar_meta2 = tmp_meta;
    }

    for (i = 0; i < 2; i++) {
      guint8 *afd_bar_data_ptr;

      if (i == 0) {
        afd_bar_data_ptr = afd_bar_data;
        afd = afd_meta ? afd_meta->afd : 0;
        is_letterbox = bar_meta ? bar_meta->is_letterbox : FALSE;
        bar1 = bar_meta ? bar_meta->bar_data1 : 0;
        bar2 = bar_meta ? bar_meta->bar_data2 : 0;
      } else {
        afd_bar_data_ptr = afd_bar_data2;
        afd = afd_meta2 ? afd_meta2->afd : 0;
        is_letterbox = bar_meta2 ? bar_meta2->is_letterbox : FALSE;
        bar1 = bar_meta2 ? bar_meta2->bar_data1 : 0;
        bar2 = bar_meta2 ? bar_meta2->bar_data2 : 0;
      }

      /* See SMPTE 2016-3 Section 4 */
      /* AFD and AR */
      if (self->mode <= (gint) GST_DECKLINK_MODE_PAL_P) {
        afd_bar_data_ptr[0] = (afd << 3) | 0x0;
      } else {
        afd_bar_data_ptr[0] = (afd << 3) | 0x4;
      }

      /* Bar flags */
      afd_bar_data_ptr[3] = is_letterbox ? 0xc0 : 0x30;

      /* Bar value 1 and 2 */
      GST_WRITE_UINT16_BE (&afd_bar_data_ptr[4], bar1);
      GST_WRITE_UINT16_BE (&afd_bar_data_ptr[6], bar2);
    }

    /* AFD on the same line as the captions */
    if (self->caption_line == self->afd_bar_line) {
      if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");
    }

    /* FIXME: Add captions to the correct field? Captions for the second
     * field should probably be inserted into the second field */

    if (got_captions || self->caption_line == self->afd_bar_line) {
      if (vanc_frame->GetBufferForVerticalBlankingLine (self->caption_line,
              (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbiencoder, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data",
            self->caption_line);
      }
    }

    /* AFD on a different line than the captions */
    if (self->afd_bar_line != 0 && self->caption_line != self->afd_bar_line) {
      if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");

      if (vanc_frame->GetBufferForVerticalBlankingLine (self->afd_bar_line,
              (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbiencoder, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data",
            self->afd_bar_line);
      }
    }

    /* For interlaced video we need to also add AFD to the second field */
    if (GST_VIDEO_INFO_IS_INTERLACED (&self->info) && self->afd_bar_line != 0) {
      guint field2_offset;

      /* The VANC lines for the second field are at an offset, depending on
       * the format in use.
       */
      switch (self->info.height) {
        case 486:
          /* NTSC: 525 / 2 + 1 */
          field2_offset = 263;
          break;
        case 576:
          /* PAL: 625 / 2 + 1 */
          field2_offset = 313;
          break;
        case 1080:
          /* 1080i: 1125 / 2 + 1 */
          field2_offset = 563;
          break;
        default:
          g_assert_not_reached ();
      }

      if (!gst_video_vbi_encoder_add_ancillary (self->vbiencoder,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data2,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");

      if (vanc_frame->GetBufferForVerticalBlankingLine (self->afd_bar_line +
              field2_offset, (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbiencoder, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data",
            self->afd_bar_line);
      }
    }

    if (frame->SetAncillaryData (vanc_frame) != S_OK) {
      GST_WARNING_OBJECT (self, "Failed to set ancillary data");
    }

    vanc_frame->Release ();
  } else if (got_captions || self->afd_bar_line != 0) {
    GST_WARNING_OBJECT (self, "Failed to allocate ancillary data frame");
  }
}

static GstFlowReturn
schedule_video_frame (GstDecklinkSink * self, GstBuffer * buffer,
    GstClockTime timestamp, GstClockTime duration)
{
  GstVideoFrame vframe;
  IDeckLinkMutableVideoFrame *frame;
  guint8 *outdata, *indata;
  GstDecklinkVideoFormat caps_format;
  BMDPixelFormat format;
  gint stride;
  HRESULT ret;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  gint i;
  GstVideoTimeCodeMeta *tc_meta;

  caps_format = gst_decklink_type_from_video_format (self->info.finfo->format);
  format = gst_decklink_pixel_format_from_type (caps_format);

  ret = self->output->output->CreateVideoFrame (self->info.width,
      self->info.height, self->info.stride[0], format, bmdFrameFlagDefault,
      &frame);
  if (ret != S_OK) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        (NULL), ("Failed to create video frame: 0x%08lx", (unsigned long) ret));
    return GST_FLOW_ERROR;
  }

  if (!gst_video_frame_map (&vframe, &self->info, buffer, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map video frame");
    flow_ret = GST_FLOW_ERROR;
    goto out;
  }

  frame->GetBytes ((void **) &outdata);
  indata = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
  stride =
      MIN (GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0), frame->GetRowBytes ());
  for (i = 0; i < self->info.height; i++) {
    memcpy (outdata, indata, stride);
    indata += GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
    outdata += frame->GetRowBytes ();
  }
  gst_video_frame_unmap (&vframe);

  tc_meta = gst_buffer_get_video_time_code_meta (buffer);
  if (tc_meta) {
    BMDTimecodeFlags bflags = (BMDTimecodeFlags) 0;
    gchar *tc_str;

    if (((GstVideoTimeCodeFlags) (tc_meta->tc.config.
                flags)) & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME)
      bflags = (BMDTimecodeFlags) (bflags | bmdTimecodeIsDropFrame);
    else
      bflags = (BMDTimecodeFlags) (bflags | bmdTimecodeFlagDefault);
    if (tc_meta->tc.field_count == 2)
      bflags = (BMDTimecodeFlags) (bflags | bmdTimecodeFieldMark);

    tc_str = gst_video_time_code_to_string (&tc_meta->tc);
    ret = frame->SetTimecodeFromComponents (self->timecode_format,
        (uint8_t) tc_meta->tc.hours,
        (uint8_t) tc_meta->tc.minutes,
        (uint8_t) tc_meta->tc.seconds, (uint8_t) tc_meta->tc.frames, bflags);
    if (ret != S_OK) {
      GST_ERROR_OBJECT (self,
          "Failed to set timecode %s to video frame: 0x%08lx", tc_str,
          (unsigned long) ret);
      flow_ret = GST_FLOW_ERROR;
      g_free (tc_str);
      goto out;
    }
    GST_DEBUG_OBJECT (self, "Set frame timecode to %s", tc_str);
    g_free (tc_str);
  }

  write_vbi (self, buffer, format, frame, tc_meta);

  GST_LOG_OBJECT (self, "Scheduling %p at %" GST_TIME_FORMAT " with duration: %" GST_TIME_FORMAT, frame,
      GST_TIME_ARGS (timestamp), GST_TIME_ARGS (duration));

  ret = self->output->output->ScheduleVideoFrame (frame,
      timestamp, duration, GST_SECOND);

  if (ret != S_OK) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        (NULL), ("Failed to schedule frame: 0x%08lx", (unsigned long) ret));
    flow_ret = GST_FLOW_ERROR;
    goto out;
  }

out:
  frame->Release ();

  return flow_ret;
}

static GstFlowReturn
schedule_audio_samples (GstDecklinkSink * self, GstBuffer * buffer,
    GstClockTime timestamp)
{
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstVideoAudioMeta *ameta;
  GstMapInfo amap;
  guint32 n_audio_frames;
  HRESULT ret;
  guint8 *data;

  ameta = gst_buffer_get_video_audio_meta (buffer);

  if (ameta == NULL) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        (NULL), ("Audio meta is required"));
    flow_ret = GST_FLOW_ERROR;
    goto out;
  }

  if (self->n_prerolled_frames == 0) {
    GST_INFO_OBJECT (self, "Beginning audio preroll");

    ret = self->output->output->BeginAudioPreroll ();

    if (ret != S_OK) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          (NULL), ("Failed to begin audio preroll: 0x%08lx",
              (unsigned long) ret));
      flow_ret = GST_FLOW_ERROR;
      goto out;
    }
  }

  if (!gst_buffer_map (ameta->buffer, &amap, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        (NULL), ("Failed to map audio buffer"));
    flow_ret = GST_FLOW_ERROR;
    goto out;
  }

  n_audio_frames = amap.size / 4 / self->audio_channels;

  data = amap.data;

  while (n_audio_frames > 0) {
    guint32 written = 0;

    GST_LOG_OBJECT (self, "Writing %u audio frames", n_audio_frames);

    ret =
        self->output->output->ScheduleAudioSamples ((void *) data,
          n_audio_frames, 0, GST_SECOND, &written);
    GST_LOG_OBJECT (self, "Wrote %u audio frames", written);
    data += written * 4 * self->audio_channels;
    n_audio_frames -= written;

    if (ret != S_OK) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          (NULL), ("Failed to schedule audio samples: 0x%08lx",
              (unsigned long) ret));
      flow_ret = GST_FLOW_ERROR;
      goto unmap;
    }
  }

unmap:
  gst_buffer_unmap (ameta->buffer, &amap);

out:
  return flow_ret;
}

static GstFlowReturn
schedule_buffer (GstDecklinkSink *self, GstBuffer *buffer)
{
  GstFlowReturn flow_ret;
  GstClockTime timestamp, next_timestamp, duration;

  timestamp =
      gst_util_uint64_scale (self->n_frames, self->info.fps_d * GST_SECOND,
      self->info.fps_n);

  self->n_frames++;

  next_timestamp =
      gst_util_uint64_scale (self->n_frames, self->info.fps_d * GST_SECOND,
      self->info.fps_n);

  duration = next_timestamp - timestamp;

  gst_buffer_replace (&self->last_buffer, buffer);

  if ((flow_ret =
          schedule_video_frame (self, buffer, timestamp,
              duration)) != GST_FLOW_OK) {
    goto out;
  }

  if ((flow_ret =
          schedule_audio_samples (self, buffer, timestamp)) != GST_FLOW_OK) {
    goto out;
  }

out:
  return flow_ret;
}

static GstFlowReturn
gst_decklink_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (bsink);
  GstFlowReturn flow_ret;
  HRESULT ret;
  bool active;

  /* FIXME: error out properly */
  g_assert (self->min_buffered_frames < self->n_preroll_frames);
  g_assert (self->n_preroll_frames < self->max_buffered_frames);

  g_mutex_lock(&self->schedule_lock);

  GST_DEBUG_OBJECT (self, "Preparing buffer %" GST_PTR_FORMAT, buffer);

  ret = self->output->output->IsScheduledPlaybackRunning (&active);

  if (ret != S_OK) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED,
        (NULL),
        ("Failed to determine if scheduled playback is running: 0x%08lx",
            (unsigned long) ret));
    flow_ret = GST_FLOW_ERROR;
    goto out;
  }

  if (active) {
    uint32_t bufferedFrameCount;

    if (self->output->output->
        GetBufferedVideoFrameCount (&bufferedFrameCount) != S_OK) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          (NULL),
          ("Failed to determine how many frames are current buffered: 0x%08lx",
              (unsigned long) ret));
      flow_ret = GST_FLOW_ERROR;
      goto out;
    }

    if (bufferedFrameCount > (uint32_t) self->max_buffered_frames) {
      GST_WARNING_OBJECT(self, "Skipping frame as we have exceeded the max buffered frames threshold");
      flow_ret = GST_FLOW_OK;
      goto out;
    }
  }

  if ((flow_ret =
          schedule_buffer (self, buffer)) != GST_FLOW_OK) {
    goto out;
  }

  self->n_prerolled_frames += 1;

  if (!active && self->n_prerolled_frames >= self->n_preroll_frames) {
    GST_INFO_OBJECT (self, "Ending audio preroll");

    ret = self->output->output->EndAudioPreroll ();

    if (ret != S_OK) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          (NULL), ("Failed to end audio preroll: 0x%08lx",
              (unsigned long) ret));
      flow_ret = GST_FLOW_ERROR;
      goto out;
    }

    GST_INFO_OBJECT (self, "Starting scheduled playback");

    ret =
        self->output->output->StartScheduledPlayback (0,
        GST_SECOND, 1.0);

    if (ret != S_OK) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          (NULL), ("Failed to start playback: 0x%08lx", (unsigned long) ret));
      flow_ret = GST_FLOW_ERROR;
      goto out;
    }
  }

  flow_ret = GST_FLOW_OK;

out:
  g_mutex_unlock(&self->schedule_lock);

  return flow_ret;
}

static gboolean
gst_decklink_sink_open (GstBaseSink * bsink)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (bsink);
  const GstDecklinkMode *mode;

  self->output =
      gst_decklink_acquire_nth_output (self->device_number,
      GST_ELEMENT_CAST (self), FALSE);
  if (!self->output) {
    GST_ERROR_OBJECT (self, "Failed to acquire output");
    return FALSE;
  }

  g_object_notify (G_OBJECT (self), "hw-serial-number");

  mode = gst_decklink_get_mode (self->mode);
  g_assert (mode != NULL);

  self->output->mode = mode;

  self->n_prerolled_frames = 0;
  self->vbiencoder = NULL;
  self->anc_vformat = GST_VIDEO_FORMAT_UNKNOWN;
  self->cdp_hdr_sequence_cntr = 0;

  self->last_buffer = NULL;
  self->n_frames = 0;

  return TRUE;
}

static void
_wait_for_stop_notify (GstDecklinkSink * self)
{
  bool active = false;

  self->output->output->IsScheduledPlaybackRunning (&active);
  while (active) {
    /* cause sometimes decklink stops without notifying us... */
    guint64 wait_time = g_get_monotonic_time () + G_TIME_SPAN_SECOND;
    if (!g_cond_wait_until (&self->output->cond, &self->output->lock,
            wait_time))
      GST_WARNING_OBJECT (self, "Failed to wait for stop notification");
    self->output->output->IsScheduledPlaybackRunning (&active);
  }
}

static gboolean
gst_decklink_sink_close (GstBaseSink * bsink)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (bsink);

  GST_INFO_OBJECT (self, "Closing");

  if (self->output) {
    bool active;

    active = false;
    self->output->output->IsScheduledPlaybackRunning (&active);
    if (active) {
      HRESULT res;
      GST_DEBUG_OBJECT (self, "Stopping scheduled playback");

      g_mutex_lock(&self->schedule_lock);
      res = self->output->output->StopScheduledPlayback (0, 0, 0);
      g_mutex_unlock(&self->schedule_lock);
      if (res != S_OK) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED,
            (NULL), ("Failed to stop scheduled playback: 0x%08lx",
                (unsigned long) res));
        return FALSE;
      }
      // Wait until scheduled playback actually stopped
      _wait_for_stop_notify (self);
    }

    self->output->mode = NULL;
    self->output->video_enabled = FALSE;
    self->output->audio_enabled = FALSE;

    self->output->output->DisableVideoOutput ();
    self->output->output->DisableAudioOutput ();
    gst_decklink_release_nth_output (self->device_number,
        GST_ELEMENT_CAST (self), FALSE);
    self->output = NULL;
  }

  if (self->vbiencoder) {
    gst_video_vbi_encoder_free (self->vbiencoder);
    self->vbiencoder = NULL;
  }

  gst_buffer_replace(&self->last_buffer, NULL);

  return TRUE;
}

static GstStateChangeReturn
gst_decklink_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstDecklinkSink *self = GST_DECKLINK_SINK_CAST (element);
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  GST_DEBUG_OBJECT (self, "changing state: %s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  return ret;
}

static gboolean
gst_decklink_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
    {
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
}

static gboolean
gst_decklink_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps == NULL)
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

    if (!gst_buffer_pool_set_config (pool, structure))
      goto config_failed;

    gst_query_add_allocation_pool (query, pool, size, 0, 0);
    gst_object_unref (pool);
    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  }

  return TRUE;
  // ERRORS
config_failed:
  {
    GST_ERROR_OBJECT (bsink, "failed to set config");
    gst_object_unref (pool);
    return FALSE;
  }
}
