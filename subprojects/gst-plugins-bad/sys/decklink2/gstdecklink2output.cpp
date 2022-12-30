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

#include "gstdecklink2output.h"
#include "gstdecklink2object.h"
#include <gst/base/base.h>

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_output_debug);
#define GST_CAT_DEFAULT gst_decklink2_output_debug

class IGstDeckLinkVideoOutputCallback;

struct GstDeckLink2OutputPrivate
{
  std::mutex extern_lock;
  std::mutex schedule_lock;
  std::condition_variable cond;
};

struct _GstDeckLink2Output
{
  GstObject parent;

  GstDeckLink2OutputPrivate *priv;

  GstDeckLink2APILevel api_level;

  IDeckLink *device;
  IDeckLinkProfileAttributes *attr;
  IDeckLinkAttributes_v10_11 *attr_10_11;
  IDeckLinkConfiguration *config;
  IDeckLinkConfiguration_v10_11 *config_10_11;
  IDeckLinkKeyer *keyer;

  IDeckLinkOutput *output;
  IDeckLinkOutput_v11_4 *output_11_4;
  IDeckLinkOutput_v10_11 *output_10_11;

  IGstDeckLinkVideoOutputCallback *callback;
  IDeckLinkVideoFrame *last_frame;

  GstCaps *caps;
  GArray *format_table;
  guint max_audio_channels;

  GstDeckLink2DisplayMode selected_mode;
  GstAudioInfo audio_info;

  GstVideoVBIEncoder *vbi_enc;
  gint vbi_width;
  guint16 cdp_hdr_sequence_cntr;
  guint n_prerolled;
  guint64 n_frames;
  guint n_preroll_frames;
  guint min_buffered;
  guint max_buffered;
  GstClockTime pts;

  gboolean configured;
  gboolean prerolled;
};

static void gst_decklink2_output_dispose (GObject * object);
static void gst_decklink2_output_finalize (GObject * object);
static void gst_decklink2_output_on_stopped (GstDeckLink2Output * self);
static void gst_decklink2_output_on_completed (GstDeckLink2Output * self);

#define gst_decklink2_output_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Output, gst_decklink2_output, GST_TYPE_OBJECT);

class IGstDeckLinkVideoOutputCallback:public IDeckLinkVideoOutputCallback
{
public:
  IGstDeckLinkVideoOutputCallback (GstDeckLink2Output * output)
  :ref_count_ (1), output_ (output)
  {
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkVideoOutputCallback) {
      *object = static_cast < IDeckLinkVideoOutputCallback * >(this);
    } else {
      *object = NULL;
      return E_NOINTERFACE;
    }

    AddRef ();

    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef (void)
  {
    ULONG cnt = ref_count_.fetch_add (1);

    return cnt + 1;
  }

  ULONG STDMETHODCALLTYPE Release (void)
  {
    ULONG cnt = ref_count_.fetch_sub (1);
    if (cnt == 1)
      delete this;

    return cnt - 1;
  }

  /* IDeckLinkVideoOutputCallback */
  HRESULT STDMETHODCALLTYPE
      ScheduledFrameCompleted (IDeckLinkVideoFrame * frame,
      BMDOutputFrameCompletionResult result)
  {
    switch (result) {
      case bmdOutputFrameCompleted:
        GST_LOG_OBJECT (output_, "Completed frame %p", frame);
        break;
      case bmdOutputFrameDisplayedLate:
        GST_WARNING_OBJECT (output_, "Late Frame %p", frame);
        break;
      case bmdOutputFrameDropped:
        GST_WARNING_OBJECT (output_, "Dropped Frame %p", frame);
        break;
      case bmdOutputFrameFlushed:
        GST_LOG_OBJECT (output_, "Flushed Frame %p", frame);
        break;
      default:
        GST_WARNING_OBJECT (output_, "Unknown Frame %p: %d",
            frame, (gint) result);
        break;
    }

    if (result == bmdOutputFrameCompleted ||
        result == bmdOutputFrameDisplayedLate) {
      gst_decklink2_output_on_completed (output_);
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped (void)
  {
    gst_decklink2_output_on_stopped (output_);
    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkVideoOutputCallback () {
  }

private:
  std::atomic < ULONG > ref_count_;
  GstDeckLink2Output *output_;
};

static void
gst_decklink2_output_class_init (GstDeckLink2OutputClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_decklink2_output_dispose;
  object_class->finalize = gst_decklink2_output_finalize;

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_output_debug, "decklink2output",
      0, "decklink2output");
}

static void
gst_decklink2_output_init (GstDeckLink2Output * self)
{
  self->priv = new GstDeckLink2OutputPrivate ();
  self->format_table = g_array_new (FALSE,
      FALSE, sizeof (GstDeckLink2DisplayMode));
  self->callback = new IGstDeckLinkVideoOutputCallback (self);
}

static void
gst_decklink2_output_dispose (GObject * object)
{
  GstDeckLink2Output *self = GST_DECKLINK2_OUTPUT (object);

  gst_clear_caps (&self->caps);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_decklink2_output_finalize (GObject * object)
{
  GstDeckLink2Output *self = GST_DECKLINK2_OUTPUT (object);

  GST_DECKLINK2_CLEAR_COM (self->output);
  GST_DECKLINK2_CLEAR_COM (self->output_11_4);
  GST_DECKLINK2_CLEAR_COM (self->output_10_11);
  GST_DECKLINK2_CLEAR_COM (self->keyer);
  GST_DECKLINK2_CLEAR_COM (self->attr);
  GST_DECKLINK2_CLEAR_COM (self->attr_10_11);
  GST_DECKLINK2_CLEAR_COM (self->config);
  GST_DECKLINK2_CLEAR_COM (self->config_10_11);
  GST_DECKLINK2_CLEAR_COM (self->device);
  GST_DECKLINK2_CLEAR_COM (self->callback);

  g_array_unref (self->format_table);
  g_clear_pointer (&self->vbi_enc, gst_video_vbi_encoder_free);

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* Returns true if format is supported without conversion */
static gboolean
gst_decklink2_output_does_support_video_mode (GstDeckLink2Output * self,
    BMDDisplayMode mode, BMDPixelFormat format)
{
  HRESULT hr;
  BMDDisplayModeSupport_v10_11 supported_v10_11 =
      bmdDisplayModeNotSupported_v10_11;
  dlbool_t supported = (dlbool_t) 0;
  BMDDisplayMode actual_mode = mode;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->DoesSupportVideoMode (mode, format,
          bmdVideoOutputFlagDefault, &supported_v10_11, NULL);
      if (hr != S_OK || supported_v10_11 != bmdDisplayModeSupported_v10_11)
        return FALSE;
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->
          output_11_4->DoesSupportVideoMode (bmdVideoConnectionUnspecified,
          mode, format, bmdSupportedVideoModeDefault, &actual_mode, &supported);
      if (hr != S_OK || !supported || actual_mode != mode)
        return FALSE;
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->DoesSupportVideoMode (bmdVideoConnectionUnspecified,
          mode, format, bmdNoVideoOutputConversion,
          bmdSupportedVideoModeDefault, &actual_mode, &supported);
      if (hr != S_OK || !supported || actual_mode != mode)
        return FALSE;
      break;
    default:
      g_assert_not_reached ();
      return FALSE;
  }

  return TRUE;
}

static IDeckLinkDisplayModeIterator *
gst_decklink2_output_get_iterator (GstDeckLink2Output * self)
{
  IDeckLinkDisplayModeIterator *iter = NULL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      self->output_10_11->GetDisplayModeIterator (&iter);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      self->output_11_4->GetDisplayModeIterator (&iter);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      self->output->GetDisplayModeIterator (&iter);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return iter;
}

GstDeckLink2Output *
gst_decklink2_output_new (IDeckLink * device, GstDeckLink2APILevel api_level)
{
  GstDeckLink2Output *self;
  IDeckLinkDisplayModeIterator *iter = NULL;
  HRESULT hr;

  if (!device || api_level == GST_DECKLINK2_API_LEVEL_UNKNOWN)
    return NULL;

  self = (GstDeckLink2Output *) g_object_new (GST_TYPE_DECKLINK2_OUTPUT, NULL);
  self->api_level = api_level;
  self->device = device;
  device->AddRef ();
  device->QueryInterface (IID_IDeckLinkKeyer, (void **) &self->keyer);

  if (api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    device->QueryInterface (IID_IDeckLinkAttributes_v10_11,
        (void **) &self->attr_10_11);
  } else {
    device->QueryInterface (IID_IDeckLinkProfileAttributes,
        (void **) &self->attr);
  }

  self->max_audio_channels = 2;
  gint64 max_audio_channels = 2;
  if (self->attr) {
    hr = self->attr->GetInt (BMDDeckLinkMaximumAudioChannels,
        &max_audio_channels);
    if (gst_decklink2_result (hr))
      self->max_audio_channels = (guint) max_audio_channels;
  } else if (self->attr_10_11) {
    hr = self->attr_10_11->GetInt (BMDDeckLinkMaximumAudioChannels,
        &max_audio_channels);
    if (gst_decklink2_result (hr))
      self->max_audio_channels = (guint) max_audio_channels;
  }

  if (api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    device->QueryInterface (IID_IDeckLinkConfiguration_v10_11,
        (void **) &self->config_10_11);
  } else {
    device->QueryInterface (IID_IDeckLinkConfiguration,
        (void **) &self->config);
  }

  switch (api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = device->QueryInterface (IID_IDeckLinkOutput_v10_11,
          (void **) &self->output_10_11);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = device->QueryInterface (IID_IDeckLinkOutput_v11_4,
          (void **) &self->output_11_4);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }

      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = device->QueryInterface (IID_IDeckLinkOutput,
          (void **) &self->output);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    default:
      g_assert_not_reached ();
      gst_object_unref (self);
      return NULL;
  }

  iter = gst_decklink2_output_get_iterator (self);
  if (!iter) {
    gst_object_unref (self);
    return NULL;
  }

  self->caps = gst_decklink2_build_template_caps (GST_OBJECT (self), iter,
      (GstDeckLink2DoesSupportVideoMode)
      gst_decklink2_output_does_support_video_mode, self->format_table);
  iter->Release ();

  if (!self->caps)
    gst_clear_object (&self);

  return self;
}

GstCaps *
gst_decklink2_output_get_caps (GstDeckLink2Output * output, BMDDisplayMode mode,
    BMDPixelFormat format)
{
  IDeckLinkDisplayModeIterator *iter = NULL;
  GstCaps *caps;

  if (mode == bmdModeUnknown && format == bmdFormatUnspecified)
    return gst_caps_ref (output->caps);

  iter = gst_decklink2_output_get_iterator (output);
  if (!iter)
    return NULL;

  caps = gst_decklink2_build_caps (GST_OBJECT (output), iter,
      mode, format, (GstDeckLink2DoesSupportVideoMode)
      gst_decklink2_output_does_support_video_mode);
  iter->Release ();

  return caps;
}

gboolean
gst_decklink2_output_get_display_mode (GstDeckLink2Output * output,
    const GstVideoInfo * info, GstDeckLink2DisplayMode * display_mode)
{
  for (guint i = 0; i < output->format_table->len; i++) {
    const GstDeckLink2DisplayMode *m = &g_array_index (output->format_table,
        GstDeckLink2DisplayMode, i);

    if (m->width == info->width && m->height == info->height &&
        m->fps_n == info->fps_n && m->fps_d == info->fps_d &&
        m->par_n == info->par_n && m->par_d == info->par_d) {
      if ((m->interlaced && !GST_VIDEO_INFO_IS_INTERLACED (info)) ||
          (!m->interlaced && GST_VIDEO_INFO_IS_INTERLACED (info))) {
        continue;
      }

      *display_mode = *m;
      return TRUE;
    }
  }

  return FALSE;
}

guint
gst_decklink2_output_get_max_audio_channels (GstDeckLink2Output * output)
{
  return output->max_audio_channels;
}

static HRESULT
gst_decklink2_output_enable_video (GstDeckLink2Output * self,
    BMDDisplayMode mode, BMDVideoOutputFlags flags)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->EnableVideoOutput (mode, flags);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->EnableVideoOutput (mode, flags);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->EnableVideoOutput (mode, flags);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_enable_audio (GstDeckLink2Output * self,
    BMDAudioSampleRate rate, BMDAudioSampleType sample_type, guint channels,
    BMDAudioOutputStreamType stream_type)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->EnableAudioOutput (rate,
          sample_type, channels, stream_type);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->EnableAudioOutput (rate,
          sample_type, channels, stream_type);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->EnableAudioOutput (rate,
          sample_type, channels, stream_type);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_disable_video (GstDeckLink2Output * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->DisableVideoOutput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->DisableVideoOutput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->DisableVideoOutput ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_disable_audio (GstDeckLink2Output * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->DisableAudioOutput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->DisableAudioOutput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->DisableAudioOutput ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_begin_audio_preroll (GstDeckLink2Output * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->BeginAudioPreroll ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->BeginAudioPreroll ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->BeginAudioPreroll ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_end_audio_preroll (GstDeckLink2Output * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->EndAudioPreroll ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->EndAudioPreroll ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->EndAudioPreroll ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_schedule_audio_samples (GstDeckLink2Output * self,
    void *buffer, guint count, BMDTimeValue stream_time,
    BMDTimeScale scale, guint * written)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->ScheduleAudioSamples (buffer, count, stream_time,
          scale, written);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->ScheduleAudioSamples (buffer, count, stream_time,
          scale, written);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->ScheduleAudioSamples (buffer, count, stream_time,
          scale, written);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_set_video_callback (GstDeckLink2Output * self,
    IDeckLinkVideoOutputCallback * callback)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->SetScheduledFrameCompletionCallback (callback);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->SetScheduledFrameCompletionCallback (callback);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->SetScheduledFrameCompletionCallback (callback);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_create_ancillary_data (GstDeckLink2Output * self,
    BMDPixelFormat format, IDeckLinkVideoFrameAncillary ** frame)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->CreateAncillaryData (format, frame);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->CreateAncillaryData (format, frame);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->CreateAncillaryData (format, frame);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_is_running (GstDeckLink2Output * self, dlbool_t * active)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->IsScheduledPlaybackRunning (active);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->IsScheduledPlaybackRunning (active);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->IsScheduledPlaybackRunning (active);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_get_num_bufferred (GstDeckLink2Output * self,
    guint32 * count)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->GetBufferedVideoFrameCount (count);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->GetBufferedVideoFrameCount (count);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->GetBufferedVideoFrameCount (count);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_start (GstDeckLink2Output * self,
    BMDTimeValue start_time, BMDTimeScale scale, double speed)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->StartScheduledPlayback (start_time,
          scale, speed);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->StartScheduledPlayback (start_time, scale, speed);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->StartScheduledPlayback (start_time, scale, speed);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_output_schedule_video_internal (GstDeckLink2Output * self,
    IDeckLinkVideoFrame * frame, guint8 * audio_buf, gsize audio_buf_size)
{
  GstClockTime next_pts, dur;
  HRESULT hr = E_FAIL;

  frame->AddRef ();
  GST_DECKLINK2_CLEAR_COM (self->last_frame);
  self->last_frame = frame;

  self->n_frames++;
  next_pts = gst_util_uint64_scale (self->n_frames,
      self->selected_mode.fps_d * GST_SECOND, self->selected_mode.fps_n);
  dur = next_pts - self->pts;

  GST_LOG_OBJECT (self, "Scheduling video frame %p, audio-buf-size %"
      G_GSIZE_FORMAT, frame, audio_buf_size);

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->ScheduleVideoFrame (frame,
          self->pts, dur, GST_SECOND);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->ScheduleVideoFrame (frame,
          self->pts, dur, GST_SECOND);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->ScheduleVideoFrame (frame, self->pts, dur, GST_SECOND);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  self->pts = next_pts;
  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (self, "Couldn't schedule video frame, hr: 0x%x",
        (guint) hr);
    return hr;
  }

  if (!self->prerolled) {
    if (self->n_prerolled == 0 && GST_AUDIO_INFO_IS_VALID (&self->audio_info)) {
      GST_DEBUG_OBJECT (self, "Begin audio preroll");

      hr = gst_decklink2_output_begin_audio_preroll (self);
      if (!gst_decklink2_result (hr)) {
        GST_ERROR_OBJECT (self, "Couldn't start audio preroll, hr: 0x%x",
            (guint) hr);
        return hr;
      }
    }

    if (audio_buf && audio_buf_size > 0) {
      guint num_samples = audio_buf_size / self->audio_info.bpf;

      hr = gst_decklink2_output_schedule_audio_samples (self,
          audio_buf, num_samples, 0, 0, NULL);
      if (!gst_decklink2_result (hr)) {
        GST_ERROR_OBJECT (self, "Couldn't schedule audio sample, hr: 0x%x",
            (guint) hr);
        return hr;
      }
    }

    self->n_prerolled++;
    if (self->n_prerolled >= self->n_preroll_frames) {
      if (GST_AUDIO_INFO_IS_VALID (&self->audio_info)) {
        hr = gst_decklink2_output_end_audio_preroll (self);

        if (!gst_decklink2_result (hr)) {
          GST_ERROR_OBJECT (self, "Audio preroll failed, hr: 0x%x", (guint) hr);
          return hr;
        }
      }

      hr = gst_decklink2_output_start (self, 0, GST_SECOND, 1.0);
      if (!gst_decklink2_result (hr)) {
        GST_ERROR_OBJECT (self, "Couldn't start playback, hr: 0x%x",
            (guint) hr);
        return hr;
      }

      GST_DEBUG_OBJECT (self, "Prerolled, start playback");

      self->prerolled = TRUE;
    }
  } else if (audio_buf && audio_buf_size > 0) {
    guint num_samples = audio_buf_size / self->audio_info.bpf;

    hr = gst_decklink2_output_schedule_audio_samples (self,
        audio_buf, num_samples, 0, 0, NULL);
    if (!gst_decklink2_result (hr)) {
      GST_ERROR_OBJECT (self, "Couldn't schedule audio sample, hr: 0x%x",
          (guint) hr);
      return hr;
    }
  }

  return S_OK;
}

HRESULT
gst_decklink2_output_schedule_stream (GstDeckLink2Output * output,
    IDeckLinkVideoFrame * frame, guint8 * audio_buf, gsize audio_buf_size)
{
  GstDeckLink2OutputPrivate *priv = output->priv;
  HRESULT hr;
  std::unique_lock < std::mutex > lk (priv->extern_lock);
  dlbool_t active;

  g_assert (output->configured);

  hr = gst_decklink2_output_is_running (output, &active);
  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (output, "Couldn't query active state, hr: 0x%x",
        (guint) hr);
    return hr;
  }

  if (active) {
    guint32 count = 0;
    hr = gst_decklink2_output_get_num_bufferred (output, &count);
    if (!gst_decklink2_result (hr)) {
      GST_ERROR_OBJECT (output,
          "Couldn't query bufferred frame count, hr: 0x%x", (guint) hr);
      return hr;
    }

    if (count > output->max_buffered) {
      GST_WARNING_OBJECT (output, "Skipping frame, buffered count %u > %u",
          count, output->max_buffered);
      return S_OK;
    }
  }
  lk.unlock ();

  std::lock_guard < std::mutex > slk (priv->schedule_lock);
  return gst_decklink2_output_schedule_video_internal (output, frame,
      audio_buf, audio_buf_size);
}

static HRESULT
gst_decklink2_output_stop_internal (GstDeckLink2Output * self)
{
  GstDeckLink2OutputPrivate *priv = self->priv;
  HRESULT hr = E_FAIL;

  GST_DEBUG_OBJECT (self, "Stopping");

  std::unique_lock < std::mutex > lk (priv->schedule_lock);
  /* Steal last frame to avoid re-rendering */
  GST_DECKLINK2_CLEAR_COM (self->last_frame);

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->output_10_11->StopScheduledPlayback (0, NULL, 0);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->output_11_4->StopScheduledPlayback (0, NULL, 0);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->output->StopScheduledPlayback (0, NULL, 0);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
  lk.unlock ();

  GST_DEBUG_OBJECT (self, "StopScheduledPlayback result 0x%x", (guint) hr);

  gst_decklink2_output_disable_audio (self);
  gst_decklink2_output_disable_video (self);
  gst_decklink2_output_set_video_callback (self, NULL);
  self->configured = FALSE;

  return hr;
}

HRESULT
gst_decklink2_output_stop (GstDeckLink2Output * output)
{
  GstDeckLink2OutputPrivate *priv = output->priv;
  std::lock_guard < std::mutex > lk (priv->extern_lock);

  return gst_decklink2_output_stop_internal (output);
}

class IGstDeckLinkTimecode:public IDeckLinkTimecode
{
public:
  IGstDeckLinkTimecode (GstVideoTimeCode * timecode):ref_count_ (1)
  {
    g_assert (timecode);

    timecode_ = gst_video_time_code_copy (timecode);
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkTimecode) {
      *object = static_cast < IDeckLinkTimecode * >(this);
    } else {
      *object = NULL;
      return E_NOINTERFACE;
    }

    AddRef ();

    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef (void)
  {
    ULONG cnt = ref_count_.fetch_add (1);

    return cnt + 1;
  }

  ULONG STDMETHODCALLTYPE Release (void)
  {
    ULONG cnt = ref_count_.fetch_sub (1);
    if (cnt == 1)
      delete this;

    return cnt - 1;
  }

  /* IDeckLinkTimecode */
  BMDTimecodeBCD STDMETHODCALLTYPE GetBCD (void)
  {
    BMDTimecodeBCD bcd = 0;

    bcd |= (timecode_->frames % 10) << 0;
    bcd |= ((timecode_->frames / 10) & 0x0f) << 4;
    bcd |= (timecode_->seconds % 10) << 8;
    bcd |= ((timecode_->seconds / 10) & 0x0f) << 12;
    bcd |= (timecode_->minutes % 10) << 16;
    bcd |= ((timecode_->minutes / 10) & 0x0f) << 20;
    bcd |= (timecode_->hours % 10) << 24;
    bcd |= ((timecode_->hours / 10) & 0x0f) << 28;

    if (timecode_->config.fps_n == 24 && timecode_->config.fps_d == 1)
      bcd |= 0x0 << 30;
    else if (timecode_->config.fps_n == 25 && timecode_->config.fps_d == 1)
      bcd |= 0x1 << 30;
    else if (timecode_->config.fps_n == 30 && timecode_->config.fps_d == 1001)
      bcd |= 0x2 << 30;
    else if (timecode_->config.fps_n == 30 && timecode_->config.fps_d == 1)
      bcd |= 0x3 << 30;

    return bcd;
  }

  HRESULT STDMETHODCALLTYPE
      GetComponents (unsigned char *hours, unsigned char *minutes,
      unsigned char *seconds, unsigned char *frames)
  {
    *hours = timecode_->hours;
    *minutes = timecode_->minutes;
    *seconds = timecode_->seconds;
    *frames = timecode_->frames;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetString (dlstring_t * timecode)
  {
    gchar *s = gst_video_time_code_to_string (timecode_);
    *timecode = StdToDlString (s);
    g_free (s);

    return S_OK;
  }

  BMDTimecodeFlags STDMETHODCALLTYPE GetFlags (void)
  {
    BMDTimecodeFlags flags = (BMDTimecodeFlags) 0;

    if ((timecode_->config.flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) != 0)
      flags = (BMDTimecodeFlags) (flags | bmdTimecodeIsDropFrame);
    else
      flags = (BMDTimecodeFlags) (flags | bmdTimecodeFlagDefault);

    if (timecode_->field_count == 2)
      flags = (BMDTimecodeFlags) (flags | bmdTimecodeFieldMark);

    return flags;
  }

  HRESULT STDMETHODCALLTYPE GetTimecodeUserBits (BMDTimecodeUserBits * userBits)
  {
    *userBits = 0;
    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkTimecode () {
    gst_video_time_code_free (timecode_);
  }

private:
  GstVideoTimeCode * timecode_;
  std::atomic < ULONG > ref_count_;
};

class IGstDeckLinkVideoFrame:public IDeckLinkVideoFrame
{
public:
  IGstDeckLinkVideoFrame (GstVideoFrame * frame):ref_count_ (1)
  {
    frame_ = *frame;
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkVideoOutputCallback) {
      *object = static_cast < IDeckLinkVideoFrame * >(this);
    } else {
      *object = NULL;
      return E_NOINTERFACE;
    }

    AddRef ();

    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef (void)
  {
    ULONG cnt = ref_count_.fetch_add (1);

    return cnt + 1;
  }

  ULONG STDMETHODCALLTYPE Release (void)
  {
    ULONG cnt = ref_count_.fetch_sub (1);
    if (cnt == 1)
      delete this;

    return cnt - 1;
  }

  /* IDeckLinkVideoFrame */
  long STDMETHODCALLTYPE GetWidth (void)
  {
    return GST_VIDEO_FRAME_WIDTH (&frame_);
  }

  long STDMETHODCALLTYPE GetHeight (void)
  {
    return GST_VIDEO_FRAME_HEIGHT (&frame_);
  }

  long STDMETHODCALLTYPE GetRowBytes (void)
  {
    return GST_VIDEO_FRAME_PLANE_STRIDE (&frame_, 0);
  }

  BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat (void)
  {
    BMDPixelFormat format = bmdFormatUnspecified;
    switch (GST_VIDEO_FRAME_FORMAT (&frame_)) {
      case GST_VIDEO_FORMAT_UYVY:
        format = bmdFormat8BitYUV;
        break;
      case GST_VIDEO_FORMAT_v210:
        format = bmdFormat10BitYUV;
        break;
      case GST_VIDEO_FORMAT_ARGB:
        format = bmdFormat8BitARGB;
        break;
      case GST_VIDEO_FORMAT_BGRA:
        format = bmdFormat8BitBGRA;
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    return format;
  }

  BMDFrameFlags STDMETHODCALLTYPE GetFlags (void)
  {
    return bmdFrameFlagDefault;
  }

  HRESULT STDMETHODCALLTYPE GetBytes (void **buffer)
  {
    *buffer = GST_VIDEO_FRAME_PLANE_DATA (&frame_, 0);

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
      GetTimecode (BMDTimecodeFormat format, IDeckLinkTimecode ** timecode)
  {
    if (timecode_) {
      *timecode = timecode_;
      timecode_->AddRef ();
      return S_OK;
    }

    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE
      GetAncillaryData (IDeckLinkVideoFrameAncillary ** ancillary)
  {
    if (ancillary_) {
      *ancillary = ancillary_;
      ancillary_->AddRef ();
      return S_OK;
    }

    return S_FALSE;
  }

  /* Non-interface methods */
  HRESULT STDMETHODCALLTYPE SetTimecode (GstVideoTimeCode * timecode)
  {
    GST_DECKLINK2_CLEAR_COM (timecode_);

    if (timecode)
      timecode_ = new IGstDeckLinkTimecode (timecode);

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
      SetAncillaryData (IDeckLinkVideoFrameAncillary * ancillary)
  {
    GST_DECKLINK2_CLEAR_COM (ancillary_);

    ancillary_ = ancillary;
    if (ancillary_)
      ancillary_->AddRef ();

    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkVideoFrame () {
    gst_video_frame_unmap (&frame_);
    GST_DECKLINK2_CLEAR_COM (timecode_);
    GST_DECKLINK2_CLEAR_COM (ancillary_);
  }

private:
  std::atomic < ULONG > ref_count_;
  GstVideoFrame frame_;
  IDeckLinkTimecode *timecode_ = NULL;
  IDeckLinkVideoFrameAncillary *ancillary_ = NULL;
};

/* Copied from ext/closedcaption/gstccconverter.c */
/* Converts raw CEA708 cc_data and an optional timecode into CDP */
static guint
convert_cea708_cc_data_cea708_cdp_internal (GstDeckLink2Output * self,
    const guint8 * cc_data, guint cc_data_len, guint8 * cdp, guint cdp_len,
    const GstVideoTimeCodeMeta * tc_meta)
{
  GstByteWriter bw;
  guint8 flags, checksum;
  guint i, len;
  const GstDeckLink2DisplayMode *mode = &self->selected_mode;

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
gst_decklink2_output_write_vbi (GstDeckLink2Output * self,
    const GstVideoInfo * info, GstBuffer * buffer,
    IGstDeckLinkVideoFrame * frame, GstVideoTimeCodeMeta * tc_meta,
    gint caption_line, gint afd_bar_line)
{
  IDeckLinkVideoFrameAncillary *vanc_frame = NULL;
  gpointer iter = NULL;
  GstVideoCaptionMeta *cc_meta;
  guint8 *vancdata;
  gboolean got_captions = FALSE;

  if (caption_line == 0 && afd_bar_line == 0)
    return;

  if (self->vbi_width != info->width)
    g_clear_pointer (&self->vbi_enc, gst_video_vbi_encoder_free);

  if (!self->vbi_enc) {
    self->vbi_enc = gst_video_vbi_encoder_new (GST_VIDEO_FORMAT_v210,
        info->width);
    self->vbi_width = info->width;
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
          data[3 * i] = 0x80 | (info->height ==
              525 ? caption_line - 9 : caption_line - 5);
          data[3 * i + 1] = cc_meta->data[2 * i];
          data[3 * i + 2] = cc_meta->data[2 * i + 1];
        }

        if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc, FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_608 & 0xff, data, 3)) {
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");
        }

        got_captions = TRUE;

        break;
      }
      case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:{
        if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc, FALSE,
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
        if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc, FALSE,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 >> 8,
                GST_VIDEO_ANCILLARY_DID16_S334_EIA_708 & 0xff, data, n))
          GST_WARNING_OBJECT (self, "Couldn't add meta to ancillary data");

        got_captions = TRUE;

        break;
      }
      case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:{
        if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc,
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

  if ((got_captions || afd_bar_line != 0)
      && gst_decklink2_output_create_ancillary_data (self,
          bmdFormat10BitYUV, &vanc_frame) == S_OK) {
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
      if (self->selected_mode.mode == bmdModeNTSC ||
          self->selected_mode.mode == bmdModeNTSC2398 ||
          self->selected_mode.mode == bmdModePAL ||
          self->selected_mode.mode == bmdModeNTSCp ||
          self->selected_mode.mode == bmdModePALp) {
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
    if (caption_line == afd_bar_line) {
      if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");
    }

    /* FIXME: Add captions to the correct field? Captions for the second
     * field should probably be inserted into the second field */

    if (got_captions || caption_line == afd_bar_line) {
      if (vanc_frame->GetBufferForVerticalBlankingLine (caption_line,
              (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbi_enc, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data", caption_line);
      }
    }

    /* AFD on a different line than the captions */
    if (afd_bar_line != 0 && caption_line != afd_bar_line) {
      if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");

      if (vanc_frame->GetBufferForVerticalBlankingLine (afd_bar_line,
              (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbi_enc, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data", afd_bar_line);
      }
    }

    /* For interlaced video we need to also add AFD to the second field */
    if (GST_VIDEO_INFO_IS_INTERLACED (info) && afd_bar_line != 0) {
      guint field2_offset;

      /* The VANC lines for the second field are at an offset, depending on
       * the format in use.
       */
      switch (info->height) {
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

      if (!gst_video_vbi_encoder_add_ancillary (self->vbi_enc,
              FALSE, GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR >> 8,
              GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR & 0xff, afd_bar_data2,
              sizeof (afd_bar_data)))
        GST_WARNING_OBJECT (self,
            "Couldn't add AFD/Bar data to ancillary data");

      if (vanc_frame->GetBufferForVerticalBlankingLine (afd_bar_line +
              field2_offset, (void **) &vancdata) == S_OK) {
        gst_video_vbi_encoder_write_line (self->vbi_enc, vancdata);
      } else {
        GST_WARNING_OBJECT (self,
            "Failed to get buffer for line %d ancillary data", afd_bar_line);
      }
    }

    if (frame->SetAncillaryData (vanc_frame) != S_OK) {
      GST_WARNING_OBJECT (self, "Failed to set ancillary data");
    }

    vanc_frame->Release ();
  } else if (got_captions || afd_bar_line != 0) {
    GST_WARNING_OBJECT (self, "Failed to allocate ancillary data frame");
  }
}

IDeckLinkVideoFrame *
gst_decklink2_output_upload (GstDeckLink2Output * output,
    const GstVideoInfo * info, GstBuffer * buffer, gint caption_line,
    gint afd_bar_line)
{
  GstVideoFrame vframe;
  IGstDeckLinkVideoFrame *frame;
  GstVideoTimeCodeMeta *tc_meta;

  if (!gst_video_frame_map (&vframe, info, buffer, GST_MAP_READ)) {
    GST_ERROR_OBJECT (output, "Failed to map video frame");
    return NULL;
  }

  frame = new IGstDeckLinkVideoFrame (&vframe);

  tc_meta = gst_buffer_get_video_time_code_meta (buffer);
  if (tc_meta)
    frame->SetTimecode (&tc_meta->tc);

  gst_decklink2_output_write_vbi (output, info, buffer, frame, tc_meta,
      caption_line, afd_bar_line);

  return frame;
}

HRESULT
gst_decklink2_output_configure (GstDeckLink2Output * output,
    guint n_preroll_frames, guint min_buffered, guint max_buffered,
    const GstDeckLink2DisplayMode * display_mode,
    BMDVideoOutputFlags output_flags, BMDProfileID profile_id,
    GstDeckLink2KeyerMode keyer_mode, guint8 keyer_level,
    GstDeckLink2MappingFormat mapping_format,
    BMDAudioSampleType audio_sample_type, guint audio_channels)
{
  GstDeckLink2OutputPrivate *priv = output->priv;
  HRESULT hr = S_OK;

  std::lock_guard < std::mutex > lk (priv->extern_lock);
  if (output->configured)
    gst_decklink2_output_stop_internal (output);

  output->selected_mode = *display_mode;

  if (profile_id != bmdProfileDefault) {
    GstDeckLink2Object *object;
    gchar *profile_id_str = g_enum_to_string (GST_TYPE_DECKLINK2_PROFILE_ID,
        profile_id);

    object = (GstDeckLink2Object *) gst_object_get_parent (GST_OBJECT (output));
    g_assert (object);

    gst_decklink2_object_set_profile_id (object, profile_id);
    gst_object_unref (object);

    g_free (profile_id_str);
  }

  if (mapping_format != GST_DECKLINK2_MAPPING_FORMAT_DEFAULT &&
      (output->attr || output->attr_10_11)) {
    dlbool_t supported = false;

    if (output->attr) {
      hr = output->attr->GetFlag (BMDDeckLinkSupportsSMPTELevelAOutput,
          &supported);
    } else {
      hr = output->attr_10_11->GetFlag (BMDDeckLinkSupportsSMPTELevelAOutput,
          &supported);
    }

    if (gst_decklink2_result (hr) && supported) {
      hr = E_FAIL;
      if (mapping_format == GST_DECKLINK2_MAPPING_FORMAT_LEVEL_A) {
        if (output->config_10_11) {
          hr = output->
              config_10_11->SetFlag (bmdDeckLinkConfigSMPTELevelAOutput, true);
        } else if (output->config) {
          hr = output->config->SetFlag (bmdDeckLinkConfigSMPTELevelAOutput,
              true);
        }
      } else if (mapping_format == GST_DECKLINK2_MAPPING_FORMAT_LEVEL_B) {
        if (output->config_10_11) {
          hr = output->
              config_10_11->SetFlag (bmdDeckLinkConfigSMPTELevelAOutput, false);
        } else if (output->config) {
          hr = output->config->SetFlag (bmdDeckLinkConfigSMPTELevelAOutput,
              false);
        }
      }

      if (gst_decklink2_result (hr))
        GST_DEBUG_OBJECT (output, "SMPTELevelAOutput is configured");
      else
        GST_WARNING_OBJECT (output, "Couldn't configure SMPTELevelAOutput");
    } else {
      GST_WARNING_OBJECT (output, "SMPTELevelAOutput is not supported");
    }
  }

  if (output->keyer) {
    switch (keyer_mode) {
      case GST_DECKLINK2_KEYER_MODE_INTERNAL:
        output->keyer->Enable (false);
        output->keyer->SetLevel (keyer_level);
        break;
      case GST_DECKLINK2_KEYER_MODE_EXTERNAL:
        output->keyer->Enable (true);
        output->keyer->SetLevel (keyer_level);
        break;
      case GST_DECKLINK2_KEYER_MODE_OFF:
      default:
        output->keyer->Disable ();
        break;
    }
  } else if (keyer_mode != GST_DECKLINK2_KEYER_MODE_OFF) {
    GST_WARNING_OBJECT (output, "Keyer interface is unavailable");
  }

  hr = gst_decklink2_output_enable_video (output,
      gst_decklink2_get_real_display_mode (output->selected_mode.mode),
      output_flags);
  if (!gst_decklink2_result (hr))
    goto error;

  hr = gst_decklink2_output_set_video_callback (output, output->callback);
  if (!gst_decklink2_result (hr))
    goto error;

  gst_audio_info_init (&output->audio_info);
  if (audio_channels > 0) {
    GST_DEBUG_OBJECT (output, "Enabling audio");
    hr = gst_decklink2_output_enable_audio (output, bmdAudioSampleRate48kHz,
        audio_sample_type, audio_channels, bmdAudioOutputStreamContinuous);
    if (!gst_decklink2_result (hr))
      goto error;

    gst_audio_info_set_format (&output->audio_info,
        audio_sample_type == bmdAudioSampleType16bitInteger ?
        GST_AUDIO_FORMAT_S16LE : GST_AUDIO_FORMAT_S32LE, 48000,
        audio_channels, NULL);
  }

  g_clear_pointer (&output->vbi_enc, gst_video_vbi_encoder_free);

  output->n_prerolled = 0;
  output->n_preroll_frames = n_preroll_frames;
  output->min_buffered = min_buffered;
  output->max_buffered = max_buffered;
  output->n_frames = 0;
  output->cdp_hdr_sequence_cntr = 0;
  output->configured = TRUE;
  output->pts = 0;

  return S_OK;

error:
  gst_decklink2_output_disable_audio (output);
  gst_decklink2_output_disable_video (output);
  gst_decklink2_output_set_video_callback (output, NULL);
  return hr;
}

static void
gst_decklink2_output_on_stopped (GstDeckLink2Output * self)
{
  GST_DEBUG_OBJECT (self, "Scheduled playback stopped");
}

static void
gst_decklink2_output_on_completed (GstDeckLink2Output * self)
{
  GstDeckLink2OutputPrivate *priv = self->priv;
  dlbool_t active;
  guint32 count = 0;

  std::lock_guard < std::mutex > lk (priv->schedule_lock);
  if (!self->last_frame)
    return;

  HRESULT hr = gst_decklink2_output_is_running (self, &active);
  if (gst_decklink2_result (hr) && active) {
    hr = gst_decklink2_output_get_num_bufferred (self, &count);
    if (gst_decklink2_result (hr) && count <= self->min_buffered) {
      GST_WARNING_OBJECT (self, "Underrun, buffered count %u", count);
      gst_decklink2_output_schedule_video_internal (self, self->last_frame,
          NULL, 0);
    }
  }
}
