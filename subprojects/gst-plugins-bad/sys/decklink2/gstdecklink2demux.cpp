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

#include "gstdecklink2demux.h"
#include "gstdecklink2utils.h"
#include "gstdecklink2object.h"

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_demux_debug);
#define GST_CAT_DEFAULT gst_decklink2_demux_debug

static GstStaticPadTemplate audio_template = GST_STATIC_PAD_TEMPLATE ("audio",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("audio/x-raw, format = (string) { S16LE, S32LE }, "
        "rate = (int) 48000, channels = (int) { 2, 8, 16 }, "
        "layout = (string) interleaved"));

struct _GstDeckLink2Demux
{
  GstElement parent;

  GstPad *sink_pad;
  GstPad *video_pad;
  GstPad *audio_pad;
  GstVideoInfo video_info;

  GstFlowCombiner *flow_combiner;
  GstCaps *audio_caps;

  guint drop_count;
};

static void gst_decklink2_demux_finalize (GObject * object);
static GstStateChangeReturn
gst_decklink2_demux_change_state (GstElement * element,
    GstStateChange transition);
static GstFlowReturn gst_decklink2_demux_chain (GstPad * sinkpad,
    GstObject * parent, GstBuffer * inbuf);
static gboolean gst_decklink2_demux_sink_event (GstPad * sinkpad,
    GstObject * parent, GstEvent * event);

#define gst_decklink2_demux_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Demux, gst_decklink2_demux, GST_TYPE_ELEMENT);
GST_ELEMENT_REGISTER_DEFINE (decklink2demux, "decklink2demux",
    GST_RANK_NONE, GST_TYPE_DECKLINK2_DEMUX);

static void
gst_decklink2_demux_class_init (GstDeckLink2DemuxClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  object_class->finalize = gst_decklink2_demux_finalize;

  GstCaps *templ_caps = gst_decklink2_get_default_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, templ_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("video", GST_PAD_SRC, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref (templ_caps);
  gst_element_class_add_static_pad_template (element_class, &audio_template);

  gst_element_class_set_static_metadata (element_class,
      "Decklink2 Demux", "Video/Audio/Demuxer/Hardware", "Decklink2 Demux",
      "Seungha Yang <seungha@centricular.com>");

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_decklink2_demux_change_state);

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_demux_debug, "decklink2demux",
      0, "decklink2demux");
}

static void
gst_decklink2_demux_init (GstDeckLink2Demux * self)
{
  GstPadTemplate *templ;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (self);

  templ = gst_element_class_get_pad_template (klass, "sink");
  self->sink_pad = gst_pad_new_from_template (templ, "sink");
  gst_pad_set_chain_function (self->sink_pad, gst_decklink2_demux_chain);
  gst_pad_set_event_function (self->sink_pad, gst_decklink2_demux_sink_event);
  gst_element_add_pad (GST_ELEMENT (self), self->sink_pad);
  gst_object_unref (templ);

  templ = gst_element_class_get_pad_template (klass, "video");
  self->video_pad = gst_pad_new_from_template (templ, "video");
  gst_element_add_pad (GST_ELEMENT (self), self->video_pad);
  gst_pad_use_fixed_caps (self->video_pad);
  gst_object_unref (templ);

  self->flow_combiner = gst_flow_combiner_new ();
  gst_flow_combiner_add_pad (self->flow_combiner, self->video_pad);
}

static void
gst_decklink2_demux_finalize (GObject * object)
{
  GstDeckLink2Demux *self = GST_DECKLINK2_DEMUX (object);

  gst_flow_combiner_free (self->flow_combiner);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_decklink2_demux_change_state (GstElement * element,
    GstStateChange transition)
{
  GstDeckLink2Demux *self = GST_DECKLINK2_DEMUX (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_clear_caps (&self->audio_caps);
      self->drop_count = 0;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->audio_pad) {
        gst_flow_combiner_remove_pad (self->flow_combiner, self->audio_pad);
        gst_element_remove_pad (element, self->audio_pad);
        self->audio_pad = NULL;
      }
      gst_clear_caps (&self->audio_caps);
      break;
    default:
      break;
  }

  return ret;
}

static GstFlowReturn
gst_decklink2_demux_chain (GstPad * sinkpad, GstObject * parent,
    GstBuffer * inbuf)
{
  GstDeckLink2Demux *self = GST_DECKLINK2_DEMUX (parent);
  GstDeckLink2AudioMeta *meta;
  GstSample *audio_sample = NULL;
  GstFlowReturn ret;
  gsize buf_size;

  meta = gst_buffer_get_decklink2_audio_meta (inbuf);
  if (meta) {
    audio_sample = gst_sample_ref (meta->sample);
    inbuf = gst_buffer_make_writable (inbuf);
    gst_buffer_remove_meta (inbuf, GST_META_CAST (meta));
  }

  if (audio_sample) {
    GstCaps *audio_caps = gst_sample_get_caps (audio_sample);
    GstBuffer *audio_buf = gst_sample_get_buffer (audio_sample);

    if (!audio_caps) {
      GST_WARNING_OBJECT (self, "Audio sample without caps");
      gst_sample_unref (audio_sample);
      audio_sample = NULL;
      goto out;
    }

    if (!audio_buf) {
      GST_WARNING_OBJECT (self, "Audio sample without buffer");
      gst_sample_unref (audio_sample);
      audio_sample = NULL;
      goto out;
    }

    if (!self->audio_pad) {
      GstEvent *event;

      self->audio_pad = gst_pad_new_from_static_template (&audio_template,
          "audio");
      gst_pad_set_active (self->audio_pad, TRUE);

      event = gst_pad_get_sticky_event (self->sink_pad,
          GST_EVENT_STREAM_START, 0);

      gst_pad_store_sticky_event (self->audio_pad, event);
      gst_event_unref (event);

      gst_caps_replace (&self->audio_caps, audio_caps);

      event = gst_event_new_caps (self->audio_caps);
      gst_pad_store_sticky_event (self->audio_pad, event);
      gst_event_unref (event);

      event = gst_pad_get_sticky_event (self->sink_pad, GST_EVENT_SEGMENT, 0);
      if (event) {
        gst_pad_store_sticky_event (self->audio_pad, event);
        gst_event_unref (event);
      }

      gst_element_add_pad (GST_ELEMENT (self), self->audio_pad);
      gst_flow_combiner_add_pad (self->flow_combiner, self->audio_pad);

      gst_element_no_more_pads (GST_ELEMENT (self));
    } else if (!self->audio_caps || !gst_caps_is_equal (self->audio_caps,
            audio_caps)) {
      GstEvent *event;

      gst_caps_replace (&self->audio_caps, audio_caps);

      event = gst_event_new_caps (self->audio_caps);
      gst_pad_push_event (self->audio_pad, event);
    }
  }

out:
  buf_size = gst_buffer_get_size (inbuf);
  if (buf_size < self->video_info.size) {
    GST_WARNING_OBJECT (self, "Too small buffer size %" G_GSIZE_FORMAT
        " < %" G_GSIZE_FORMAT, buf_size, self->video_info.size);
    gst_buffer_unref (inbuf);
    self->drop_count++;

    if (self->drop_count > 30) {
      GST_ERROR_OBJECT (self, "Too many buffers were dropped");
      return GST_FLOW_ERROR;
    }

    return GST_FLOW_OK;
  }

  self->drop_count = 0;

  GST_LOG_OBJECT (self, "Pushing video buffer %" GST_PTR_FORMAT, inbuf);
  ret = gst_pad_push (self->video_pad, inbuf);
  ret = gst_flow_combiner_update_pad_flow (self->flow_combiner,
      self->video_pad, ret);

  if (audio_sample) {
    GstBuffer *audio_buf = gst_sample_get_buffer (audio_sample);
    gst_buffer_ref (audio_buf);
    gst_sample_unref (audio_sample);

    GST_LOG_OBJECT (self, "Pushing audio buffer %" GST_PTR_FORMAT, audio_buf);
    ret = gst_pad_push (self->audio_pad, audio_buf);
    ret = gst_flow_combiner_update_pad_flow (self->flow_combiner,
        self->audio_pad, ret);
  }

  return ret;
}

static gboolean
gst_decklink2_demux_sink_event (GstPad * sinkpad, GstObject * parent,
    GstEvent * event)
{
  GstDeckLink2Demux *self = GST_DECKLINK2_DEMUX (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      GST_DEBUG_OBJECT (self, "Forwarding %" GST_PTR_FORMAT, caps);

      gst_video_info_from_caps (&self->video_info, caps);

      return gst_pad_push_event (self->video_pad, event);
    }
    case GST_EVENT_FLUSH_STOP:
      gst_flow_combiner_reset (self->flow_combiner);
      break;
    default:
      break;
  }

  return gst_pad_event_default (sinkpad, parent, event);
}
