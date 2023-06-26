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

#include "gstdecklink2input.h"
#include "gstdecklink2object.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string.h>
#include <gst/base/gstqueuearray.h>

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_input_debug);
#define GST_CAT_DEFAULT gst_decklink2_input_debug

#define INVALID_AUDIO_OFFSET ((guint64) -1)

static HRESULT gst_decklink2_input_on_format_changed (GstDeckLink2Input * self,
    BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode * mode,
    BMDDetectedVideoInputFormatFlags flags);
static void gst_decklink2_input_on_frame_arrived (GstDeckLink2Input * self,
    IDeckLinkVideoInputFrame * frame, IDeckLinkAudioInputPacket * packet);

class IGstDeckLinkMemoryAllocator:public IDeckLinkMemoryAllocator
{
public:
  IGstDeckLinkMemoryAllocator ():ref_count_ (1)
  {
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkInputCallback) {
      *object = static_cast < IDeckLinkMemoryAllocator * >(this);
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

  /* IDeckLinkMemoryAllocator */
  HRESULT STDMETHODCALLTYPE AllocateBuffer (unsigned int size, void **alloced)
  {
    std::unique_lock < std::mutex > lk (lock_);
    guint8 *buf = NULL;
    guint8 offset = 0;

    if (size != last_alloc_size_) {
      GST_DEBUG ("%p size changed %u -> %u", this, last_alloc_size_, size);
      ClearPool ();
      last_alloc_size_ = size;
    }

    if (!buffers_.empty ()) {
      buf = (guint8 *) buffers_.front ();
      buffers_.pop ();
    }
    lk.unlock ();

    if (!buf) {
      buf = (uint8_t *) g_malloc (size + 128);

      /* The Decklink SDK requires 16 byte aligned memory at least but for us
       * to work nicely let's align to 64 bytes (512 bits) as this allows
       * aligned AVX2 operations for example */
      if (((guintptr) buf) % 64 != 0)
        offset = ((guintptr) buf) % 64;

      /* Write the allocation size at the very beginning. It's guaranteed by
       * malloc() to be allocated aligned enough for doing this. */
      *((guint32 *) buf) = size;

      /* Align our buffer */
      buf += 128 - offset;

      /* And write the alignment offset right before the buffer */
      *(buf - 1) = offset;
    }

    *alloced = (void *) buf;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReleaseBuffer (void *buffer)
  {
    std::lock_guard < std::mutex > lk (lock_);
    guint8 offset = *(((guint8 *) buffer) - 1);
    guint8 *alloc_buffer = ((guint8 *) buffer) - 128 + offset;
    guint32 size = *(guint32 *) alloc_buffer;

    if (!commited_ || size != last_alloc_size_)
      g_free (alloc_buffer);
    else
      buffers_.push (buffer);

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Commit (void)
  {
    std::lock_guard < std::mutex > lk (lock_);
    GST_DEBUG ("Commit %p", this);

    ClearPool ();
    commited_ = true;
    last_alloc_size_ = 0;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Decommit (void)
  {
    std::lock_guard < std::mutex > lk (lock_);

    GST_DEBUG ("Decommit %p", this);

    ClearPool ();
    commited_ = false;

    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkMemoryAllocator () {
    Decommit ();
  }

  void ClearPool (void)
  {
    while (!buffers_.empty ()) {
      guint8 *buf = (guint8 *) buffers_.front ();
      buffers_.pop ();

      guint8 offset = *(buf - 1);
      void *alloc_buf = buf - 128 + offset;
      g_free (alloc_buf);
    }
  }

private:
  std::atomic < ULONG > ref_count_;
  std::mutex lock_;
  std::queue < void *>buffers_;
  unsigned int last_alloc_size_ = 0;
  bool commited_ = false;
};


/* IDeckLinkInputCallback interface for API version < 12.0 */
class IGstDeckLinkInputCallback_v11_5_1:public IDeckLinkInputCallback_v11_5_1
{
public:
  IGstDeckLinkInputCallback_v11_5_1 (GstDeckLink2Input * input)
  :ref_count_ (1), input_ (input)
  {
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkInputCallback) {
      *object = static_cast < IGstDeckLinkInputCallback_v11_5_1 * >(this);
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

  /* IDeckLinkInputCallback_v11_5_1 */
  HRESULT STDMETHODCALLTYPE
      VideoInputFormatChanged (BMDVideoInputFormatChangedEvents events,
      IDeckLinkDisplayMode * mode, BMDDetectedVideoInputFormatFlags flags)
  {
    return gst_decklink2_input_on_format_changed (input_, events, mode, flags);
  }

  HRESULT STDMETHODCALLTYPE
      VideoInputFrameArrived (IDeckLinkVideoInputFrame * frame,
      IDeckLinkAudioInputPacket * packet)
  {
    gst_decklink2_input_on_frame_arrived (input_, frame, packet);
    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkInputCallback_v11_5_1 () {
  }

private:
  std::atomic < ULONG > ref_count_;
  GstDeckLink2Input *input_;
};

class IGstDeckLinkInputCallback:public IDeckLinkInputCallback
{
public:
  IGstDeckLinkInputCallback (GstDeckLink2Input * input)
  :ref_count_ (1), input_ (input)
  {
  }

  /* IUnknown */
  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void **object)
  {
    if (riid == IID_IDeckLinkInputCallback) {
      *object = static_cast < IDeckLinkInputCallback * >(this);
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

  /* IDeckLinkInputCallback */
  HRESULT STDMETHODCALLTYPE
      VideoInputFormatChanged (BMDVideoInputFormatChangedEvents events,
      IDeckLinkDisplayMode * mode, BMDDetectedVideoInputFormatFlags flags)
  {
    return gst_decklink2_input_on_format_changed (input_, events, mode, flags);
  }

  HRESULT STDMETHODCALLTYPE
      VideoInputFrameArrived (IDeckLinkVideoInputFrame * frame,
      IDeckLinkAudioInputPacket * packet)
  {
    gst_decklink2_input_on_frame_arrived (input_, frame, packet);
    return S_OK;
  }

private:
  virtual ~ IGstDeckLinkInputCallback () {
  }

private:
  std::atomic < ULONG > ref_count_;
  GstDeckLink2Input *input_;
};

struct GstDecklink2InputData
{
  GstBuffer *buffer;
  GstCaps *caps;
};

struct TimeMapping
{
  GstClockTime xbase;
  GstClockTime b;
  GstClockTime num;
  GstClockTime den;
};

struct GstDeckLink2InputPrivate
{
  GstDeckLink2InputPrivate ()
  {
    signal = false;
    was_restarted = false;
  }

  std::mutex lock;
  std::condition_variable cond;
  std::atomic < bool >signal;
  std::atomic < bool >was_restarted;
};

struct _GstDeckLink2Input
{
  GstObject parent;

  GstDeckLink2InputPrivate *priv;

  GstDeckLink2APILevel api_level;

  IDeckLink *device;
  IDeckLinkProfileAttributes *attr;
  IDeckLinkAttributes_v10_11 *attr_10_11;
  IDeckLinkConfiguration_v10_11 *config_10_11;
  IDeckLinkConfiguration *config;

  IDeckLinkInput *input;
  IDeckLinkInput_v11_5_1 *input_11_5_1;
  IDeckLinkInput_v11_4 *input_11_4;
  IDeckLinkInput_v10_11 *input_10_11;

  IGstDeckLinkMemoryAllocator *allocator;
  IGstDeckLinkInputCallback_v11_5_1 *callback_11_5_1;
  IGstDeckLinkInputCallback *callback;

  GstCaps *caps;
  GArray *format_table;
  GstCaps *selected_video_caps;
  GstAudioInfo audio_info;
  guint max_audio_channels;
  GstCaps *selected_audio_caps;
  gboolean auto_detect;

  guint64 next_audio_offset;
  guint64 audio_offset;
  GstAdapter *audio_buf;

  GstQueueArray *queue;

  GstDeckLink2DisplayMode selected_mode;
  BMDPixelFormat pixel_format;
  GstElement *client;
  gboolean output_cc;
  gboolean output_afd_bar;
  gint aspect_ratio_flag;
  BMDTimecodeFormat timecode_format;
  guint buffer_size;
  gboolean discont;
  gboolean audio_discont;
  gboolean flushing;
  gboolean started;
  GstClockTime skip_first_time;
  GstClockTime start_time;
  GstClockTimeDiff av_sync;

  guint window_size;
  guint window_fill;
  gboolean window_filled;
  guint window_skip;
  guint window_skip_count;
  TimeMapping current_time_mapping;
  TimeMapping next_time_mapping;
  gboolean next_time_mapping_pending;

  GstClockTime times[256];

  GstVideoVBIParser *vbi_parser;
  GstVideoFormat anc_vformat;

  gint anc_width;
  gint last_cc_vbi_line;
  gint last_cc_vbi_line_field2;
  gint last_afd_bar_vbi_line;
  gint last_afd_bar_vbi_line_field2;
};

static void gst_decklink2_input_dispose (GObject * object);
static void gst_decklink2_input_finalize (GObject * object);
static HRESULT gst_decklink2_input_set_allocator (GstDeckLink2Input * input,
    IDeckLinkMemoryAllocator * allocator);

static void
gst_decklink2_input_data_clear (GstDecklink2InputData * data)
{
  gst_clear_buffer (&data->buffer);
  gst_clear_caps (&data->caps);
}

#define gst_decklink2_input_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Input, gst_decklink2_input, GST_TYPE_OBJECT);

static void
gst_decklink2_input_class_init (GstDeckLink2InputClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_decklink2_input_dispose;
  object_class->finalize = gst_decklink2_input_finalize;

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_input_debug, "decklink2input",
      0, "decklink2input");
}

static void
gst_decklink2_input_init (GstDeckLink2Input * self)
{
  self->format_table = g_array_new (FALSE,
      FALSE, sizeof (GstDeckLink2DisplayMode));
  self->audio_buf = gst_adapter_new ();
  self->allocator = new IGstDeckLinkMemoryAllocator ();
  self->queue = gst_queue_array_new_for_struct (sizeof (GstDecklink2InputData),
      6);
  gst_queue_array_set_clear_func (self->queue,
      (GDestroyNotify) gst_decklink2_input_data_clear);
  self->priv = new GstDeckLink2InputPrivate ();
}

static void
gst_decklink2_input_dispose (GObject * object)
{
  GstDeckLink2Input *self = GST_DECKLINK2_INPUT (object);

  gst_clear_caps (&self->caps);
  gst_clear_caps (&self->selected_video_caps);
  gst_clear_caps (&self->selected_audio_caps);
  g_clear_pointer (&self->vbi_parser, gst_video_vbi_parser_free);
  g_clear_object (&self->audio_buf);
  g_clear_pointer (&self->queue, gst_queue_array_free);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_decklink2_input_finalize (GObject * object)
{
  GstDeckLink2Input *self = GST_DECKLINK2_INPUT (object);

  GST_DECKLINK2_CLEAR_COM (self->input);
  GST_DECKLINK2_CLEAR_COM (self->input_11_5_1);
  GST_DECKLINK2_CLEAR_COM (self->input_11_4);
  GST_DECKLINK2_CLEAR_COM (self->input_10_11);
  GST_DECKLINK2_CLEAR_COM (self->allocator);
  GST_DECKLINK2_CLEAR_COM (self->callback);
  GST_DECKLINK2_CLEAR_COM (self->callback_11_5_1);
  GST_DECKLINK2_CLEAR_COM (self->attr);
  GST_DECKLINK2_CLEAR_COM (self->attr_10_11);
  GST_DECKLINK2_CLEAR_COM (self->config);
  GST_DECKLINK2_CLEAR_COM (self->config_10_11);
  GST_DECKLINK2_CLEAR_COM (self->device);

  g_array_unref (self->format_table);

  delete self->priv;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* Returns true if format is supported without conversion */
static gboolean
gst_decklink2_input_does_support_video_mode (GstDeckLink2Input * self,
    BMDDisplayMode mode, BMDPixelFormat format)
{
  HRESULT hr;
  BMDDisplayModeSupport_v10_11 supported_v10_11 =
      bmdDisplayModeNotSupported_v10_11;
  dlbool_t supported = (dlbool_t) 0;
  BMDDisplayMode actual_mode = mode;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->DoesSupportVideoMode (mode, format,
          bmdVideoOutputFlagDefault, &supported_v10_11, NULL);
      if (hr != S_OK || supported_v10_11 != bmdDisplayModeSupported_v10_11)
        return FALSE;
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->
          input_11_4->DoesSupportVideoMode (bmdVideoConnectionUnspecified, mode,
          format, bmdSupportedVideoModeDefault, &supported);
      if (hr != S_OK || !supported)
        return FALSE;
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->
          input_11_5_1->DoesSupportVideoMode (bmdVideoConnectionUnspecified,
          mode, format, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault,
          &actual_mode, &supported);
      if (hr != S_OK || !supported || actual_mode != mode)
        return FALSE;
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->DoesSupportVideoMode (bmdVideoConnectionUnspecified,
          mode, format, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault,
          &actual_mode, &supported);
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
gst_decklink2_input_get_iterator (GstDeckLink2Input * self)
{
  IDeckLinkDisplayModeIterator *iter = NULL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      self->input_10_11->GetDisplayModeIterator (&iter);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      self->input_11_4->GetDisplayModeIterator (&iter);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      self->input_11_5_1->GetDisplayModeIterator (&iter);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      self->input->GetDisplayModeIterator (&iter);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return iter;
}

GstDeckLink2Input *
gst_decklink2_input_new (IDeckLink * device, GstDeckLink2APILevel api_level)
{
  GstDeckLink2Input *self;
  IDeckLinkDisplayModeIterator *iter = NULL;
  HRESULT hr;

  if (!device || api_level == GST_DECKLINK2_API_LEVEL_UNKNOWN)
    return NULL;

  self = (GstDeckLink2Input *) g_object_new (GST_TYPE_DECKLINK2_INPUT, NULL);
  self->api_level = api_level;
  self->device = device;
  device->AddRef ();

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

  if (api_level > GST_DECKLINK2_API_LEVEL_11_5_1) {
    self->callback = new IGstDeckLinkInputCallback (self);
  } else {
    self->callback_11_5_1 = new IGstDeckLinkInputCallback_v11_5_1 (self);
  }

  switch (api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = device->QueryInterface (IID_IDeckLinkInput_v10_11,
          (void **) &self->input_10_11);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = device->QueryInterface (IID_IDeckLinkInput_v11_4,
          (void **) &self->input_11_4);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = device->QueryInterface (IID_IDeckLinkInputCallback_v11_5_1,
          (void **) &self->input_11_5_1);
      if (!gst_decklink2_result (hr)) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = device->QueryInterface (IID_IDeckLinkInput, (void **) &self->input);
      if (hr != S_OK) {
        gst_object_unref (self);
        return NULL;
      }
      break;
    default:
      g_assert_not_reached ();
      gst_object_unref (self);
      return NULL;
  }

  iter = gst_decklink2_input_get_iterator (self);
  if (!iter) {
    gst_object_unref (self);
    return NULL;
  }

  hr = gst_decklink2_input_set_allocator (self, self->allocator);
  if (!gst_decklink2_result (hr)) {
    gst_object_unref (self);
    return NULL;
  }

  self->caps = gst_decklink2_build_template_caps (GST_OBJECT (self), iter,
      (GstDeckLink2DoesSupportVideoMode)
      gst_decklink2_input_does_support_video_mode, self->format_table);
  iter->Release ();

  if (!self->caps)
    gst_clear_object (&self);

  return self;
}

GstCaps *
gst_decklink2_input_get_caps (GstDeckLink2Input * input, BMDDisplayMode mode,
    BMDPixelFormat format)
{
  IDeckLinkDisplayModeIterator *iter = NULL;
  GstCaps *caps;

  if (mode == bmdModeUnknown && format == bmdFormatUnspecified)
    return gst_caps_ref (input->caps);

  iter = gst_decklink2_input_get_iterator (input);
  if (!iter)
    return NULL;

  caps = gst_decklink2_build_caps (GST_OBJECT (input), iter,
      mode, format, (GstDeckLink2DoesSupportVideoMode)
      gst_decklink2_input_does_support_video_mode);
  iter->Release ();

  return caps;
}

gboolean
gst_decklink2_input_get_display_mode (GstDeckLink2Input * input,
    const GstVideoInfo * info, GstDeckLink2DisplayMode * display_mode)
{
  for (guint i = 0; i < input->format_table->len; i++) {
    const GstDeckLink2DisplayMode *m = &g_array_index (input->format_table,
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

static gboolean
gst_decklink2_input_get_display_mode_from_native (GstDeckLink2Input * input,
    BMDDisplayMode native, GstDeckLink2DisplayMode * display_mode)
{
  for (guint i = 0; i < input->format_table->len; i++) {
    const GstDeckLink2DisplayMode *m = &g_array_index (input->format_table,
        GstDeckLink2DisplayMode, i);

    if (m->mode == native) {
      *display_mode = *m;
      return TRUE;
    }
  }

  return FALSE;
}

static HRESULT
gst_decklink2_input_set_callback (GstDeckLink2Input * self, IUnknown * callback)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->SetCallback (
          (IDeckLinkInputCallback_v11_5_1 *) callback);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->SetCallback (
          (IDeckLinkInputCallback_v11_5_1 *) callback);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->SetCallback (
          (IDeckLinkInputCallback_v11_5_1 *) callback);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->SetCallback ((IDeckLinkInputCallback *) callback);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_set_allocator (GstDeckLink2Input * self,
    IDeckLinkMemoryAllocator * allocator)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->SetVideoInputFrameMemoryAllocator (allocator);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->SetVideoInputFrameMemoryAllocator (allocator);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->SetVideoInputFrameMemoryAllocator (allocator);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->SetVideoInputFrameMemoryAllocator (allocator);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_enable_video (GstDeckLink2Input * self,
    BMDDisplayMode mode, BMDPixelFormat format, BMDVideoInputFlags flags)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->EnableVideoInput (mode, format, flags);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->EnableVideoInput (mode, format, flags);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->EnableVideoInput (mode, format, flags);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->EnableVideoInput (mode, format, flags);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_disable_video (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->DisableVideoInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->DisableVideoInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->DisableVideoInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->DisableVideoInput ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_enable_audio (GstDeckLink2Input * self,
    BMDAudioSampleRate rate, BMDAudioSampleType type, guint channels)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->EnableAudioInput (rate, type, channels);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->EnableAudioInput (rate, type, channels);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->EnableAudioInput (rate, type, channels);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->EnableAudioInput (rate, type, channels);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_disable_audio (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->DisableAudioInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->DisableAudioInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->DisableAudioInput ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->DisableAudioInput ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_start_streams (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->StartStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->StartStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->StartStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->StartStreams ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_stop_streams (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->StopStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->StopStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->StopStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->StopStreams ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_pause_streams (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->PauseStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->PauseStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->PauseStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->PauseStreams ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_flush_streams (GstDeckLink2Input * self)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->FlushStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->FlushStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->FlushStreams ();
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->FlushStreams ();
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static HRESULT
gst_decklink2_input_get_reference_clock (GstDeckLink2Input * self,
    BMDTimeScale scale, BMDTimeValue * hw_time, BMDTimeValue * time_in_frame,
    BMDTimeValue * ticks_per_frame)
{
  HRESULT hr = E_FAIL;

  switch (self->api_level) {
    case GST_DECKLINK2_API_LEVEL_10_11:
      hr = self->input_10_11->GetHardwareReferenceClock (scale,
          hw_time, time_in_frame, ticks_per_frame);
      break;
    case GST_DECKLINK2_API_LEVEL_11_4:
      hr = self->input_11_4->GetHardwareReferenceClock (scale,
          hw_time, time_in_frame, ticks_per_frame);
      break;
    case GST_DECKLINK2_API_LEVEL_11_5_1:
      hr = self->input_11_5_1->GetHardwareReferenceClock (scale,
          hw_time, time_in_frame, ticks_per_frame);
      break;
    case GST_DECKLINK2_API_LEVEL_LATEST:
      hr = self->input->GetHardwareReferenceClock (scale,
          hw_time, time_in_frame, ticks_per_frame);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return hr;
}

static void
gst_decklink2_input_reset_time_mapping (GstDeckLink2Input * self)
{
  self->window_size = 64;
  self->window_fill = 0;
  self->window_filled = FALSE;
  self->window_skip = 1;
  self->window_skip_count = 0;
  self->current_time_mapping.xbase = 0;
  self->current_time_mapping.b = 0;
  self->current_time_mapping.num = 1;
  self->current_time_mapping.den = 1;
  self->next_time_mapping.xbase = 0;
  self->next_time_mapping.b = 0;
  self->next_time_mapping.num = 1;
  self->next_time_mapping.den = 1;
}

static HRESULT
gst_decklink2_input_on_format_changed (GstDeckLink2Input * self,
    BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode * mode,
    BMDDetectedVideoInputFormatFlags flags)
{
  BMDPixelFormat pixel_format = bmdFormatUnspecified;
  GstVideoFormat video_format;
  BMDDisplayMode display_mode = mode->GetDisplayMode ();
  GstDeckLink2DisplayMode new_mode;
  GstCaps *caps;
  GstDeckLink2InputPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (self, "format changed, flags 0x%x", flags);

  if ((flags & bmdDetectedVideoInputRGB444) != 0) {
    /* XXX: cannot detect RGB format.
     * decklink SDK sample is using this value anyway */
    if ((flags & bmdDetectedVideoInput8BitDepth) != 0 ||
        flags == bmdDetectedVideoInputRGB444) {
      pixel_format = bmdFormat8BitARGB;
    }
  } else if ((flags & bmdDetectedVideoInputYCbCr422) != 0) {
    if ((flags & bmdDetectedVideoInput8BitDepth) != 0 ||
        (flags == bmdDetectedVideoInputYCbCr422)) {
      pixel_format = bmdFormat8BitYUV;
    } else if ((flags & bmdDetectedVideoInput10BitDepth) != 0) {
      pixel_format = bmdFormat10BitYUV;
    }
  }

  if (pixel_format == bmdFormatUnspecified) {
    GST_WARNING_OBJECT (self, "Unknown pixel format");
    return E_INVALIDARG;
  }

  if (!gst_decklink2_input_get_display_mode_from_native (self,
          display_mode, &new_mode)) {
    GST_WARNING_OBJECT (self, "Unknown display mode");
    return E_INVALIDARG;
  }

  video_format = gst_decklink2_video_format_from_pixel_format (pixel_format);

  caps = gst_decklink2_get_caps_from_mode (&new_mode);
  gst_caps_set_simple (caps, "format", G_TYPE_STRING,
      gst_video_format_to_string (video_format), NULL);

  GST_DEBUG_OBJECT (self, "Updated caps %" GST_PTR_FORMAT, caps);

  self->selected_mode = new_mode;
  self->pixel_format = pixel_format;

  gst_decklink2_input_pause_streams (self);
  gst_decklink2_input_enable_video (self, display_mode, pixel_format,
      bmdVideoInputEnableFormatDetection);
  gst_decklink2_input_flush_streams (self);

  gst_clear_caps (&self->selected_video_caps);
  self->selected_video_caps = caps;
  self->aspect_ratio_flag = -1;
  self->discont = TRUE;
  gst_adapter_clear (self->audio_buf);
  self->audio_offset = INVALID_AUDIO_OFFSET;
  self->next_audio_offset = INVALID_AUDIO_OFFSET;
  self->av_sync = 0;
  priv->was_restarted = true;

  gst_decklink2_input_reset_time_mapping (self);
  gst_decklink2_input_start_streams (self);

  return S_OK;
}

static void
gst_decklink2_frame_free (IDeckLinkVideoInputFrame * frame)
{
  frame->Release ();
}

static void
extract_vbi_line (GstDeckLink2Input * self, GstBuffer * buffer,
    IDeckLinkVideoFrameAncillary * vanc_frame, guint field2_offset, guint line,
    gboolean * found_cc_out, gboolean * found_afd_bar_out)
{
  GstVideoAncillary gstanc;
  const guint8 *vancdata;
  gboolean found_cc = FALSE, found_afd_bar = FALSE;

  if (vanc_frame->GetBufferForVerticalBlankingLine (field2_offset + line,
          (void **) &vancdata) != S_OK)
    return;

  GST_LOG_OBJECT (self, "Checking for VBI data on field line %u (field %u)",
      field2_offset + line, field2_offset ? 2 : 1);
  gst_video_vbi_parser_add_line (self->vbi_parser, vancdata);

  /* Check if CC or AFD/Bar is on this line if we didn't find any on a
   * previous line. Remember the line where we found them */

  while (gst_video_vbi_parser_get_ancillary (self->vbi_parser,
          &gstanc) == GST_VIDEO_VBI_PARSER_RESULT_OK) {
    switch (GST_VIDEO_ANCILLARY_DID16 (&gstanc)) {
      case GST_VIDEO_ANCILLARY_DID16_S334_EIA_708:
        if (*found_cc_out || !self->output_cc)
          continue;

        GST_LOG_OBJECT (self,
            "Adding CEA-708 CDP meta to buffer for line %u",
            field2_offset + line);
        GST_MEMDUMP_OBJECT (self, "CDP", gstanc.data, gstanc.data_count);
        gst_buffer_add_video_caption_meta (buffer,
            GST_VIDEO_CAPTION_TYPE_CEA708_CDP, gstanc.data, gstanc.data_count);

        found_cc = TRUE;
        if (field2_offset)
          self->last_cc_vbi_line_field2 = line;
        else
          self->last_cc_vbi_line = line;
        break;
      case GST_VIDEO_ANCILLARY_DID16_S334_EIA_608:
        if (*found_cc_out || !self->output_cc)
          continue;

        GST_LOG_OBJECT (self,
            "Adding CEA-608 meta to buffer for line %u", field2_offset + line);
        GST_MEMDUMP_OBJECT (self, "CEA608", gstanc.data, gstanc.data_count);
        gst_buffer_add_video_caption_meta (buffer,
            GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A, gstanc.data,
            gstanc.data_count);

        found_cc = TRUE;
        if (field2_offset)
          self->last_cc_vbi_line_field2 = line;
        else
          self->last_cc_vbi_line = line;
        break;
      case GST_VIDEO_ANCILLARY_DID16_S2016_3_AFD_BAR:{
        GstVideoAFDValue afd;
        gboolean is_letterbox;
        guint16 bar1, bar2;

        if (*found_afd_bar_out || !self->output_afd_bar)
          continue;

        GST_LOG_OBJECT (self,
            "Adding AFD/Bar meta to buffer for line %u", field2_offset + line);
        GST_MEMDUMP_OBJECT (self, "AFD/Bar", gstanc.data, gstanc.data_count);

        if (gstanc.data_count < 8) {
          GST_WARNING_OBJECT (self, "AFD/Bar data too small");
          continue;
        }

        self->aspect_ratio_flag = (gstanc.data[0] >> 2) & 0x1;

        afd = (GstVideoAFDValue) ((gstanc.data[0] >> 3) & 0xf);
        is_letterbox = ((gstanc.data[3] >> 4) & 0x3) == 0;
        bar1 = GST_READ_UINT16_BE (&gstanc.data[4]);
        bar2 = GST_READ_UINT16_BE (&gstanc.data[6]);

        gst_buffer_add_video_afd_meta (buffer, field2_offset ? 1 : 0,
            GST_VIDEO_AFD_SPEC_SMPTE_ST2016_1, afd);
        gst_buffer_add_video_bar_meta (buffer, field2_offset ? 1 : 0,
            is_letterbox, bar1, bar2);

        found_afd_bar = TRUE;
        if (field2_offset)
          self->last_afd_bar_vbi_line_field2 = line;
        else
          self->last_afd_bar_vbi_line = line;
        break;
      }
      default:
        /* otherwise continue looking */
        continue;
    }
  }

  if (found_cc)
    *found_cc_out = TRUE;
  if (found_afd_bar)
    *found_afd_bar_out = TRUE;
}

static void
extract_vbi (GstDeckLink2Input * self, GstBuffer * buffer,
    IDeckLinkVideoInputFrame * frame)
{
  IDeckLinkVideoFrameAncillary *vanc_frame = NULL;
  gint line;
  BMDPixelFormat pixel_format;
  GstVideoFormat video_format;
  gboolean found_cc = FALSE, found_afd_bar = FALSE;
  HRESULT hr;
  const GstDeckLink2DisplayMode *mode = &self->selected_mode;

  hr = frame->GetAncillaryData (&vanc_frame);
  if (!gst_decklink2_result (hr) || !vanc_frame)
    return;

  pixel_format = vanc_frame->GetPixelFormat ();
  video_format = gst_decklink2_video_format_from_pixel_format (pixel_format);
  if (video_format != GST_VIDEO_FORMAT_UYVY &&
      video_format != GST_VIDEO_FORMAT_v210) {
    GST_DEBUG_OBJECT (self, "Unknown video format for Ancillary data");
    vanc_frame->Release ();
    return;
  }

  if (video_format != self->anc_vformat || mode->width != self->anc_width)
    g_clear_pointer (&self->vbi_parser, gst_video_vbi_parser_free);

  if (!self->vbi_parser) {
    self->vbi_parser = gst_video_vbi_parser_new (video_format, mode->width);
    self->anc_vformat = video_format;
    self->anc_width = mode->width;
  }

  GST_LOG_OBJECT (self, "Checking for ancillary data in VBI");

  /* First check last known lines, if any */
  if (self->last_cc_vbi_line > 0) {
    extract_vbi_line (self, buffer, vanc_frame, 0, self->last_cc_vbi_line,
        &found_cc, &found_afd_bar);
  }
  if (self->last_afd_bar_vbi_line > 0
      && self->last_cc_vbi_line != self->last_afd_bar_vbi_line) {
    extract_vbi_line (self, buffer, vanc_frame, 0, self->last_afd_bar_vbi_line,
        &found_cc, &found_afd_bar);
  }

  if (!found_cc)
    self->last_cc_vbi_line = -1;
  if (!found_afd_bar)
    self->last_afd_bar_vbi_line = -1;

  if ((self->output_cc && !found_cc) || (self->output_afd_bar
          && !found_afd_bar)) {
    /* Otherwise loop through the first 21 lines and hope to find the data */
    /* FIXME: For the different formats the number of lines that can contain
     * VANC are different */
    for (line = 1; line < 22; line++) {
      extract_vbi_line (self, buffer, vanc_frame, 0, line, &found_cc,
          &found_afd_bar);

      /* If we found everything we wanted to extract, stop here */
      if ((!self->output_cc || found_cc) &&
          (!self->output_afd_bar || found_afd_bar))
        break;
    }
  }

  /* Do the same for field 2 in case of interlaced content */
  if (mode->interlaced) {
    gboolean found_cc_field2 = FALSE, found_afd_bar_field2 = FALSE;
    guint field2_offset = 0;

    /* The VANC lines for the second field are at an offset, depending on
     * the format in use
     */
    switch (mode->height) {
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

    /* First try the same lines as for field 1 if we don't know yet */
    if (self->last_cc_vbi_line_field2 <= 0)
      self->last_cc_vbi_line_field2 = self->last_cc_vbi_line;
    if (self->last_afd_bar_vbi_line_field2 <= 0)
      self->last_afd_bar_vbi_line_field2 = self->last_afd_bar_vbi_line;

    if (self->last_cc_vbi_line_field2 > 0) {
      extract_vbi_line (self, buffer, vanc_frame, field2_offset,
          self->last_cc_vbi_line_field2, &found_cc_field2,
          &found_afd_bar_field2);
    }
    if (self->last_afd_bar_vbi_line_field2 > 0
        && self->last_cc_vbi_line_field2 !=
        self->last_afd_bar_vbi_line_field2) {
      extract_vbi_line (self, buffer, vanc_frame, field2_offset,
          self->last_afd_bar_vbi_line_field2, &found_cc_field2,
          &found_afd_bar_field2);
    }

    if (!found_cc_field2)
      self->last_cc_vbi_line_field2 = -1;
    if (!found_afd_bar_field2)
      self->last_afd_bar_vbi_line_field2 = -1;

    if (((self->output_cc && !found_cc_field2) || (self->output_afd_bar
                && !found_afd_bar_field2))) {
      for (line = 1; line < 22; line++) {
        extract_vbi_line (self, buffer, vanc_frame, field2_offset, line,
            &found_cc_field2, &found_afd_bar_field2);

        /* If we found everything we wanted to extract, stop here */
        if ((!self->output_cc || found_cc_field2) &&
            (!self->output_afd_bar || found_afd_bar_field2))
          break;
      }
    }
  }

  vanc_frame->Release ();
}

static void
gst_decklink2_input_update_time_mapping (GstDeckLink2Input * self,
    GstClockTime capture_time, GstClockTime stream_time)
{
  if (self->window_skip_count == 0) {
    GstClockTime num, den, b, xbase;
    gdouble r_squared;

    self->times[2 * self->window_fill] = stream_time;
    self->times[2 * self->window_fill + 1] = capture_time;

    self->window_fill++;
    self->window_skip_count++;
    if (self->window_skip_count >= self->window_skip)
      self->window_skip_count = 0;

    if (self->window_fill >= self->window_size) {
      guint fps =
          ((gdouble) self->selected_mode.fps_n + self->selected_mode.fps_d -
          1) / ((gdouble) self->selected_mode.fps_d);

      /* Start by updating first every frame, once full every second frame,
       * etc. until we update once every 4 seconds */
      if (self->window_skip < 4 * fps)
        self->window_skip *= 2;
      if (self->window_skip >= 4 * fps)
        self->window_skip = 4 * fps;

      self->window_fill = 0;
      self->window_filled = TRUE;
    }

    /* First sample ever, create some basic mapping to start */
    if (!self->window_filled && self->window_fill == 1) {
      self->current_time_mapping.xbase = stream_time;
      self->current_time_mapping.b = capture_time;
      self->current_time_mapping.num = 1;
      self->current_time_mapping.den = 1;
      self->next_time_mapping_pending = FALSE;
    }

    /* Only bother calculating anything here once we had enough measurements,
     * i.e. let's take the window size as a start */
    if (self->window_filled &&
        gst_calculate_linear_regression (self->times, &self->times[128],
            self->window_size, &num, &den, &b, &xbase, &r_squared)) {

      GST_LOG_OBJECT (self,
          "Calculated new time mapping: pipeline time = %lf * (stream time - %"
          G_GUINT64_FORMAT ") + %" G_GUINT64_FORMAT " (%lf)",
          ((gdouble) num) / ((gdouble) den), xbase, b, r_squared);

      self->next_time_mapping.xbase = xbase;
      self->next_time_mapping.b = b;
      self->next_time_mapping.num = num;
      self->next_time_mapping.den = den;
      self->next_time_mapping_pending = TRUE;
    }
  } else {
    self->window_skip_count++;
    if (self->window_skip_count >= self->window_skip)
      self->window_skip_count = 0;
  }

  if (self->next_time_mapping_pending) {
    GstClockTime expected, new_calculated, diff, max_diff;

    expected =
        gst_clock_adjust_with_calibration (NULL, stream_time,
        self->current_time_mapping.xbase, self->current_time_mapping.b,
        self->current_time_mapping.num, self->current_time_mapping.den);
    new_calculated =
        gst_clock_adjust_with_calibration (NULL, stream_time,
        self->next_time_mapping.xbase, self->next_time_mapping.b,
        self->next_time_mapping.num, self->next_time_mapping.den);

    if (new_calculated > expected)
      diff = new_calculated - expected;
    else
      diff = expected - new_calculated;

    /* At most 5% frame duration change per update */
    max_diff =
        gst_util_uint64_scale (GST_SECOND / 20, self->selected_mode.fps_d,
        self->selected_mode.fps_n);

    GST_LOG_OBJECT (self,
        "New time mapping causes difference of %" GST_TIME_FORMAT,
        GST_TIME_ARGS (diff));
    GST_LOG_OBJECT (self, "Maximum allowed per frame %" GST_TIME_FORMAT,
        GST_TIME_ARGS (max_diff));

    if (diff > max_diff) {
      /* adjust so that we move that much closer */
      if (new_calculated > expected) {
        self->current_time_mapping.b = expected + max_diff;
        self->current_time_mapping.xbase = stream_time;
      } else {
        self->current_time_mapping.b = expected - max_diff;
        self->current_time_mapping.xbase = stream_time;
      }
    } else {
      self->current_time_mapping.xbase = self->next_time_mapping.xbase;
      self->current_time_mapping.b = self->next_time_mapping.b;
      self->current_time_mapping.num = self->next_time_mapping.num;
      self->current_time_mapping.den = self->next_time_mapping.den;
      self->next_time_mapping_pending = FALSE;
    }
  }
}

static void
gst_decklink2_input_on_frame_arrived (GstDeckLink2Input * self,
    IDeckLinkVideoInputFrame * frame, IDeckLinkAudioInputPacket * packet)
{
  GstDeckLink2InputPrivate *priv = self->priv;
  static GstStaticCaps stream_reference =
      GST_STATIC_CAPS ("timestamp/x-decklink-stream");
  static GstStaticCaps hardware_reference =
      GST_STATIC_CAPS ("timestamp/x-decklink-hardware");
  HRESULT hr;
  GstClockTime capture_time = 0;
  GstClockTime base_time = 0;
  GstBuffer *buffer = NULL;
  GstClock *clock = NULL;
  BMDTimeValue hw_now, dummy, dummy2;
  BMDTimeValue stream_time = GST_CLOCK_TIME_NONE;
  BMDTimeValue stream_dur;
  BMDFrameFlags flags = bmdFrameFlagDefault;

  if (frame) {
    if (priv->was_restarted) {
      /* Ignores no-signal flag of the first frame after we do restart */
      priv->was_restarted = false;
    } else {
      flags = frame->GetFlags ();
      if ((flags & bmdFrameHasNoInputSource) != 0) {
        GST_DEBUG_OBJECT (self, "No signal");
        priv->signal = false;
      } else if (!priv->signal) {
        GST_INFO_OBJECT (self, "Got first frame, reset timing map");
        priv->signal = true;
        priv->was_restarted = true;
        gst_decklink2_input_reset_time_mapping (self);
        gst_decklink2_input_stop_streams (self);
        gst_decklink2_input_flush_streams (self);
        gst_decklink2_input_start_streams (self);
        gst_adapter_clear (self->audio_buf);
        self->audio_offset = INVALID_AUDIO_OFFSET;
        self->next_audio_offset = INVALID_AUDIO_OFFSET;
        return;
      } else {
        priv->signal = true;
      }
    }
  }

  std::unique_lock < std::mutex > lk (priv->lock);
  hr = gst_decklink2_input_get_reference_clock (self, GST_SECOND,
      &hw_now, &dummy, &dummy2);
  if (!gst_decklink2_result (hr)) {
    GST_WARNING_OBJECT (self, "Couldn't get H/W reference clock");
    hw_now = GST_CLOCK_TIME_NONE;
  }

  if (self->client)
    clock = gst_element_get_clock (GST_ELEMENT (self->client));

  if (!clock) {
    GST_WARNING_OBJECT (self,
        "Frame arrived but we dont have configured clock");
    return;
  }

  base_time = gst_element_get_base_time (GST_ELEMENT (self->client));
  capture_time = gst_clock_get_time (clock);
  gst_object_unref (clock);
  if (capture_time >= base_time)
    capture_time -= base_time;

  if (!GST_CLOCK_TIME_IS_VALID (self->start_time))
    self->start_time = capture_time;

  if (GST_CLOCK_TIME_IS_VALID (self->skip_first_time)) {
    GstClockTime diff = capture_time - self->start_time;
    if (diff < self->skip_first_time) {
      GST_DEBUG_OBJECT (self,
          "Skipping frame as requested: %" GST_TIME_FORMAT " < %"
          GST_TIME_FORMAT, GST_TIME_ARGS (capture_time),
          GST_TIME_ARGS (self->skip_first_time + self->start_time));
      return;
    }

    GST_DEBUG_OBJECT (self, "All frames were skipped as requested");
    self->skip_first_time = GST_CLOCK_TIME_NONE;
  }

  if (frame) {
    void *frame_data;
    gsize frame_size;
    BMDTimeValue frame_time, frame_dur;
    IDeckLinkTimecode *timecode = NULL;
    GstClockTime pts, dur;

    hr = frame->GetBytes (&frame_data);
    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Couldn't get byte from frame");
      return;
    }

    frame_size = frame->GetHeight () * frame->GetRowBytes ();
    buffer = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
        frame_data, frame_size, 0, frame_size, frame,
        (GDestroyNotify) gst_decklink2_frame_free);
    frame->AddRef ();

    hr = frame->GetHardwareReferenceTimestamp (GST_SECOND,
        &frame_time, &frame_dur);
    if (gst_decklink2_result (hr)) {
      GstCaps *caps = gst_static_caps_get (&hardware_reference);
      gst_buffer_add_reference_timestamp_meta (buffer, caps, frame_time,
          frame_dur);
      if (GST_CLOCK_TIME_IS_VALID (hw_now) && hw_now > frame_time) {
        GstClockTime diff = hw_now - frame_time;
        if (capture_time >= diff)
          capture_time -= diff;
      }
    }

    hr = frame->GetStreamTime (&stream_time, &stream_dur, GST_SECOND);
    if (gst_decklink2_result (hr)) {
      GstCaps *caps = gst_static_caps_get (&stream_reference);
      gst_buffer_add_reference_timestamp_meta (buffer, caps, stream_time,
          stream_dur);

      gst_decklink2_input_update_time_mapping (self, capture_time, stream_time);
      pts = gst_clock_adjust_with_calibration (NULL, stream_time,
          self->current_time_mapping.xbase, self->current_time_mapping.b,
          self->current_time_mapping.num, self->current_time_mapping.den);
      dur = gst_util_uint64_scale (stream_dur,
          self->current_time_mapping.num, self->current_time_mapping.den);
    } else {
      pts = capture_time;
      dur = GST_CLOCK_TIME_NONE;
    }

    if (!priv->signal) {
      GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_GAP);
      GST_DEBUG_OBJECT (self, "No signal");
    } else {
      if (self->output_cc || self->output_afd_bar) {
        extract_vbi (self, buffer, frame);

        if (self->aspect_ratio_flag == 1 && self->auto_detect) {
          BMDDisplayMode mode = self->selected_mode.mode;

          switch (self->selected_mode.mode) {
            case bmdModeNTSC:
              mode = bmdModeNTSC_W;
              break;
            case bmdModeNTSC2398:
              mode = bmdModeNTSC2398_W;
              break;
            case bmdModePAL:
              mode = bmdModePAL_W;
              break;
            case bmdModeNTSCp:
              mode = bmdModeNTSCp_W;
              break;
            case bmdModePALp:
              mode = bmdModePALp_W;
              break;
            default:
              break;
          }

          if (mode != self->selected_mode.mode) {
            GstDeckLink2DisplayMode new_mode;
            GstVideoFormat video_format;
            GstCaps *caps;
            gst_decklink2_input_get_display_mode_from_native (self,
                mode, &new_mode);

            video_format =
                gst_decklink2_video_format_from_pixel_format
                (self->pixel_format);
            caps = gst_decklink2_get_caps_from_mode (&new_mode);
            gst_caps_set_simple (caps, "format", G_TYPE_STRING,
                gst_video_format_to_string (video_format), NULL);

            GST_DEBUG_OBJECT (self, "Update caps %" GST_PTR_FORMAT " to %"
                GST_PTR_FORMAT, self->selected_video_caps, caps);
            self->selected_mode = new_mode;
            gst_caps_replace (&self->selected_video_caps, caps);
            gst_caps_unref (caps);
          }
        }
      }
    }

    hr = frame->GetTimecode (self->timecode_format, &timecode);
    if (hr == S_OK) {
      guint8 h, m, s, f;
      hr = timecode->GetComponents (&h, &m, &s, &f);
      if (gst_decklink2_result (hr)) {
        GstVideoTimeCodeFlags tc_flags = GST_VIDEO_TIME_CODE_FLAGS_NONE;
        GstVideoTimeCode tc;

        if (self->selected_mode.interlaced) {
          tc_flags = (GstVideoTimeCodeFlags) (tc_flags |
              GST_VIDEO_TIME_CODE_FLAGS_INTERLACED);
        }

        if (self->selected_mode.fps_d == 1001 &&
            (self->selected_mode.fps_n == 30000 ||
                self->selected_mode.fps_d == 60000)) {
          tc_flags = (GstVideoTimeCodeFlags) (tc_flags |
              GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME);
        }

        gst_video_time_code_init (&tc, self->selected_mode.fps_n,
            self->selected_mode.fps_d, NULL, tc_flags, h, m, s, f, 0);
        gst_buffer_add_video_time_code_meta (buffer, &tc);
        gst_video_time_code_clear (&tc);
      }

      timecode->Release ();
    }

    if (self->selected_mode.interlaced) {
      GST_BUFFER_FLAG_SET (buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      if (self->selected_mode.tff)
        GST_BUFFER_FLAG_SET (buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    }

    if (self->discont) {
      GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
      self->discont = FALSE;
    }

    GST_BUFFER_DTS (buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_PTS (buffer) = pts;
    GST_BUFFER_DURATION (buffer) = dur;
  }

  if (packet) {
    GstBuffer *audio_buf = NULL;
    void *packet_data;
    BMDTimeValue packet_time;
    guint64 audio_offset, audio_offset_end;
    gsize audio_buf_size;
    GstMapInfo map;

    if (self->audio_offset == INVALID_AUDIO_OFFSET && !frame) {
      GST_DEBUG_OBJECT (self, "Drop audio without video frame");
      goto out;
    }

    long sample_count = packet->GetSampleFrameCount ();
    if (sample_count == 0) {
      GST_DEBUG_OBJECT (self, "Empty audio packet");
      goto out;
    }

    hr = packet->GetPacketTime (&packet_time, self->audio_info.rate);
    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Unknown audio packet time");
      goto out;
    }

    hr = packet->GetBytes (&packet_data);
    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Couldn't get audio packet data");
      goto out;
    }

    audio_offset = packet_time;
    audio_offset_end = audio_offset + sample_count;
    audio_buf_size = self->audio_info.bpf * sample_count;
    audio_buf = gst_buffer_new_and_alloc (audio_buf_size);
    gst_buffer_map (audio_buf, &map, GST_MAP_WRITE);
    memcpy (map.data, packet_data, map.size);
    gst_buffer_unmap (audio_buf, &map);

    if (self->audio_offset == INVALID_AUDIO_OFFSET) {
      GstClockTime audio_pts;
      GstClockTime packet_time_in_gst;

      packet_time_in_gst = gst_util_uint64_scale (GST_SECOND, packet_time,
          self->audio_info.rate);

      audio_pts = gst_clock_adjust_with_calibration (NULL,
          packet_time_in_gst,
          self->current_time_mapping.xbase, self->current_time_mapping.b,
          self->current_time_mapping.num, self->current_time_mapping.den);

      /* Back to sample offset */
      self->audio_offset = gst_util_uint64_scale (audio_pts,
          self->audio_info.rate, GST_SECOND);

      GST_DEBUG_OBJECT (self, "Initial audio offset at %" G_GUINT64_FORMAT
          " for pts %" GST_TIME_FORMAT ", packet time %" GST_TIME_FORMAT,
          self->audio_offset, GST_TIME_ARGS (audio_pts),
          GST_TIME_ARGS (packet_time_in_gst));
    }

    if (self->next_audio_offset == INVALID_AUDIO_OFFSET) {
      self->next_audio_offset = audio_offset_end;
    } else if (self->next_audio_offset != audio_offset) {
      GST_WARNING_OBJECT (self, "Expected offset %" G_GUINT64_FORMAT
          ", received %" G_GUINT64_FORMAT, self->next_audio_offset,
          audio_offset);
      self->audio_discont = TRUE;

      if (self->next_audio_offset > audio_offset) {
        gsize trim = self->next_audio_offset - audio_offset;
        gsize count;

        if (trim >= (gsize) sample_count) {
          GST_WARNING_OBJECT (self, "Complately backward audio pts");
          gst_buffer_unref (audio_buf);
          goto out;
        }

        count = sample_count - trim;
        audio_buf = gst_audio_buffer_truncate (audio_buf, self->audio_info.bpf,
            trim, count);
        self->next_audio_offset += count;
      } else {
        GstBuffer *silence;
        gsize diff = audio_offset - self->next_audio_offset;

        silence = gst_buffer_new_and_alloc (diff * self->audio_info.bpf);
        gst_buffer_map (silence, &map, GST_MAP_WRITE);
        gst_audio_format_info_fill_silence (self->audio_info.finfo,
            map.data, map.size);
        gst_buffer_unmap (silence, &map);
        gst_adapter_push (self->audio_buf, silence);
        self->next_audio_offset += sample_count + diff;
      }
    } else {
      GST_LOG_OBJECT (self, "Got expected audio samples");
      self->next_audio_offset += sample_count;
    }

    if (audio_buf)
      gst_adapter_push (self->audio_buf, audio_buf);
  }

out:
  if (buffer) {
    GstSample *sample;
    gsize audio_size;
    while (gst_queue_array_get_length (self->queue) > self->buffer_size) {
      GstDecklink2InputData *data = (GstDecklink2InputData *)
          gst_queue_array_pop_head_struct (self->queue);
      gst_decklink2_input_data_clear (data);
    }

    audio_size = gst_adapter_available (self->audio_buf);
    if (audio_size > 0) {
      GstBuffer *audio_buf = gst_adapter_take_buffer (self->audio_buf,
          audio_size);
      guint64 sample_count = audio_size / self->audio_info.bpf;

      GST_BUFFER_DTS (audio_buf) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_PTS (audio_buf) =
          gst_util_uint64_scale (self->audio_offset, GST_SECOND,
          self->audio_info.rate);
      GST_BUFFER_DURATION (audio_buf) =
          gst_util_uint64_scale (sample_count, GST_SECOND,
          self->audio_info.rate);
      if (self->audio_discont) {
        GST_BUFFER_FLAG_SET (audio_buf, GST_BUFFER_FLAG_DISCONT);
        self->audio_discont = FALSE;
      }

      self->audio_offset += sample_count;

      GST_LOG_OBJECT (self, "Adding audio buffer %" GST_PTR_FORMAT, audio_buf);

      sample = gst_sample_new (audio_buf, self->selected_audio_caps, NULL,
          NULL);
      gst_buffer_add_decklink2_audio_meta (buffer, sample);
      gst_sample_unref (sample);

      if (frame && packet) {
        self->av_sync = GST_CLOCK_DIFF (GST_BUFFER_PTS (buffer),
            GST_BUFFER_PTS (audio_buf));
      }

      gst_buffer_unref (audio_buf);
    }

    GST_LOG_OBJECT (self, "Enqueue buffer %" GST_PTR_FORMAT, buffer);

    GstDecklink2InputData new_data;
    new_data.buffer = buffer;
    new_data.caps = gst_caps_ref (self->selected_video_caps);
    gst_queue_array_push_tail_struct (self->queue, &new_data);
    priv->cond.notify_all ();
  }
}

static void
gst_decklink2_input_stop_unlocked (GstDeckLink2Input * self)
{
  GstDeckLink2InputPrivate *priv = self->priv;

  gst_decklink2_input_stop_streams (self);
  gst_decklink2_input_disable_video (self);
  gst_decklink2_input_disable_audio (self);
  gst_decklink2_input_set_callback (self, NULL);
  gst_queue_array_clear (self->queue);
  gst_clear_caps (&self->selected_video_caps);
  gst_clear_caps (&self->selected_audio_caps);
  priv->signal = false;
  priv->was_restarted = false;
  self->skip_first_time = GST_CLOCK_TIME_NONE;
  self->start_time = GST_CLOCK_TIME_NONE;
  self->started = FALSE;
  self->av_sync = 0;
}

HRESULT
gst_decklink2_input_start (GstDeckLink2Input * input, GstElement * client,
    BMDProfileID profile_id, guint buffer_size, GstClockTime skip_first_time,
    const GstDeckLink2InputVideoConfig * video_config,
    const GstDeckLink2InputAudioConfig * audio_config)
{
  GstDeckLink2InputPrivate *priv = input->priv;
  std::lock_guard < std::mutex > lk (priv->lock);
  HRESULT hr;
  BMDVideoInputFlags input_flags = bmdVideoInputFlagDefault;

  gst_decklink2_input_stop_unlocked (input);
  gst_decklink2_input_reset_time_mapping (input);

  input->started = TRUE;

  if (skip_first_time > 0 && GST_CLOCK_TIME_IS_VALID (skip_first_time))
    input->skip_first_time = skip_first_time;

  if (profile_id != bmdProfileDefault) {
    GstDeckLink2Object *object;
    gchar *profile_id_str = g_enum_to_string (GST_TYPE_DECKLINK2_PROFILE_ID,
        profile_id);

    object = (GstDeckLink2Object *) gst_object_get_parent (GST_OBJECT (input));
    g_assert (object);

    gst_decklink2_object_set_profile_id (object, profile_id);
    gst_object_unref (object);

    g_free (profile_id_str);
  }

  if (video_config->connection != bmdVideoConnectionUnspecified) {
    hr = E_FAIL;
    if (input->config) {
      hr = input->config->SetInt (bmdDeckLinkConfigVideoInputConnection,
          video_config->connection);
    } else if (input->config_10_11) {
      hr = input->config_10_11->SetInt (bmdDeckLinkConfigVideoInputConnection,
          video_config->connection);
    }

    if (!gst_decklink2_result (hr)) {
      GST_ERROR_OBJECT (input, "Couldn't set video connection, hr: 0x%x",
          (guint) hr);
      return hr;
    }

    if (video_config->connection == bmdVideoConnectionComposite) {
      hr = E_FAIL;
      if (input->config) {
        hr = input->config->SetInt (bmdDeckLinkConfigAnalogVideoInputFlags,
            bmdAnalogVideoFlagCompositeSetup75);
      } else if (input->config_10_11) {
        hr = input->
            config_10_11->SetInt (bmdDeckLinkConfigAnalogVideoInputFlags,
            bmdAnalogVideoFlagCompositeSetup75);
      }

      if (!gst_decklink2_result (hr)) {
        GST_ERROR_OBJECT (input,
            "Couldn't set analog video input flags, hr: 0x%x", (guint) hr);
        return hr;
      }
    }
  }

  if (video_config->auto_detect) {
    dlbool_t auto_detect = false;

    if (input->attr) {
      hr = input->attr->GetFlag (BMDDeckLinkSupportsInputFormatDetection,
          &auto_detect);
      if (!gst_decklink2_result (hr) || !auto_detect) {
        GST_ERROR_OBJECT (input, "Auto detect is not supported");
        return E_FAIL;
      }
    } else if (input->attr_10_11) {
      hr = input->attr_10_11->GetFlag (BMDDeckLinkSupportsInputFormatDetection,
          &auto_detect);
      if (!gst_decklink2_result (hr) || !auto_detect) {
        GST_ERROR_OBJECT (input, "Auto detect is not supported");
        return E_FAIL;
      }
    } else {
      GST_ERROR_OBJECT (input,
          "IDeckLinkProfileAttributes interface is not available");
      return E_FAIL;
    }

    GST_DEBUG_OBJECT (input, "Enable format detection");

    input_flags |= bmdVideoInputEnableFormatDetection;
  }

  input->client = client;
  input->selected_mode = video_config->display_mode;
  input->pixel_format = video_config->pixel_format;
  input->output_cc = video_config->output_cc;
  input->output_afd_bar = video_config->output_afd_bar;
  input->buffer_size = buffer_size;
  input->selected_video_caps = gst_decklink2_input_get_caps (input,
      input->selected_mode.mode, video_config->pixel_format);
  if (!input->selected_video_caps) {
    GST_ERROR_OBJECT (input, "Unable to get caps from requested mode");
    goto error;
  }

  input->auto_detect = video_config->auto_detect;
  input->aspect_ratio_flag = -1;
  input->audio_offset = INVALID_AUDIO_OFFSET;
  input->next_audio_offset = INVALID_AUDIO_OFFSET;
  input->audio_discont = FALSE;

  if (input->callback)
    hr = gst_decklink2_input_set_callback (input, input->callback);
  else
    hr = gst_decklink2_input_set_callback (input, input->callback_11_5_1);

  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (input, "Couldn't set callback");
    goto error;
  }

  hr = gst_decklink2_input_enable_video (input,
      gst_decklink2_get_real_display_mode (input->selected_mode.mode),
      video_config->pixel_format, input_flags);
  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (input, "Couldn't enable video");
    goto error;
  }

  if (audio_config->channels != GST_DECKLINK2_AUDIO_CHANNELS_DISABLED) {
    guint channels = 2;
    switch (audio_config->channels) {
      case GST_DECKLINK2_AUDIO_CHANNELS_2:
        channels = 2;
        break;
      case GST_DECKLINK2_AUDIO_CHANNELS_8:
        channels = 8;
        break;
      case GST_DECKLINK2_AUDIO_CHANNELS_16:
        channels = 16;
        break;
      case GST_DECKLINK2_AUDIO_CHANNELS_MAX:
        channels = input->max_audio_channels;
        break;
      default:
        break;
    }

    hr = gst_decklink2_input_enable_audio (input, bmdAudioSampleRate48kHz,
        audio_config->sample_type, channels);
    if (!gst_decklink2_result (hr)) {
      GST_ERROR_OBJECT (input, "Couldn't enable audio");
      goto error;
    }

    gst_audio_info_set_format (&input->audio_info,
        audio_config->sample_type == bmdAudioSampleType32bitInteger ?
        GST_AUDIO_FORMAT_S32LE : GST_AUDIO_FORMAT_S16LE, 48000, channels, NULL);
    input->selected_audio_caps = gst_audio_info_to_caps (&input->audio_info);
  }

  hr = gst_decklink2_input_start_streams (input);
  if (!gst_decklink2_result (hr)) {
    GST_ERROR_OBJECT (input, "Couldn't start streams");
    goto error;
  }

  return S_OK;

error:
  gst_decklink2_input_stop_unlocked (input);
  input->client = NULL;
  return E_FAIL;
}

void
gst_decklink2_input_stop (GstDeckLink2Input * input)
{
  GstDeckLink2InputPrivate *priv = input->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  gst_decklink2_input_stop_unlocked (input);
  input->client = NULL;

  priv->cond.notify_all ();
}

void
gst_decklink2_input_set_flushing (GstDeckLink2Input * input, gboolean flush)
{
  GstDeckLink2InputPrivate *priv = input->priv;
  std::lock_guard < std::mutex > lk (priv->lock);

  input->flushing = flush;
  priv->cond.notify_all ();
}

GstFlowReturn
gst_decklink2_input_get_data (GstDeckLink2Input * input, GstBuffer ** buf,
    GstCaps ** caps, GstClockTimeDiff * av_sync)
{
  GstDeckLink2InputPrivate *priv = input->priv;
  std::unique_lock < std::mutex > lk (priv->lock);
  GstDecklink2InputData *data;

  *buf = nullptr;
  *caps = nullptr;
  *av_sync = 0;

  while (gst_queue_array_is_empty (input->queue)
      && !input->flushing && input->started) {
    priv->cond.wait (lk);
  }

  if (input->flushing)
    return GST_FLOW_FLUSHING;

  if (!input->started)
    return GST_DECKLINK2_INPUT_FLOW_STOPPED;

  data = (GstDecklink2InputData *)
      gst_queue_array_pop_head_struct (input->queue);
  *buf = data->buffer;
  *caps = data->caps;
  *av_sync = input->av_sync;

  return GST_FLOW_OK;
}

gboolean
gst_decklink2_input_has_signal (GstDeckLink2Input * input)
{
  GstDeckLink2InputPrivate *priv = input->priv;

  if (priv->signal)
    return TRUE;

  return FALSE;
}
