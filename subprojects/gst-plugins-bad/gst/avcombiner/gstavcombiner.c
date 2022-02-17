/* GStreamer
 * Copyright (C) 2021 Sebastian Dröge <sebastian@centricular.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstavcombiner.h"

GST_DEBUG_CATEGORY_STATIC (gst_av_combiner_debug);
#define GST_CAT_DEFAULT gst_av_combiner_debug

static GstStaticPadTemplate video_sink_template =
GST_STATIC_PAD_TEMPLATE ("video",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw"));

static GstStaticPadTemplate audio_sink_template =
GST_STATIC_PAD_TEMPLATE ("audio",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) S32LE, "
        "rate = (int) 48000, "
        "channels = (int) [ 1, 16 ], " "layout = (string) interleaved"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw"));

G_DEFINE_TYPE (GstAVCombiner, gst_av_combiner, GST_TYPE_AGGREGATOR);
#define parent_class gst_av_combiner_parent_class

static void
gst_av_combiner_finalize (GObject * object)
{
  GstAVCombiner *self = GST_AV_COMBINER (object);

  GST_OBJECT_LOCK (self);
  gst_caps_replace (&self->audio_caps, NULL);
  gst_caps_replace (&self->video_caps, NULL);
  GST_OBJECT_UNLOCK (self);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_av_combiner_aggregate (GstAggregator * aggregator, gboolean timeout)
{
  GstAVCombiner *self = GST_AV_COMBINER (aggregator);
  GstBuffer *video_buffer, *audio_buffer;

  if (gst_aggregator_pad_is_eos (GST_AGGREGATOR_PAD_CAST (self->audio_sinkpad))
      &&
      gst_aggregator_pad_is_eos (GST_AGGREGATOR_PAD_CAST (self->video_sinkpad)))
  {
    GST_DEBUG_OBJECT (self, "All pads EOS");
    return GST_FLOW_EOS;
  }
  // FIXME: We currently assume that upstream provides
  // - properly chunked buffers (1 buffer = 1 video frame)
  // - properly synchronized buffers (audio/video starting at the same time)
  // - no gaps
  //
  // This can be achieved externally with elements like audiobuffersplit and
  // videorate.

  video_buffer =
      gst_aggregator_pad_peek_buffer (GST_AGGREGATOR_PAD_CAST
      (self->video_sinkpad));
  if (!video_buffer)
    return GST_AGGREGATOR_FLOW_NEED_DATA;

  video_buffer = gst_buffer_make_writable (video_buffer);

  audio_buffer =
      gst_aggregator_pad_peek_buffer (GST_AGGREGATOR_PAD_CAST
      (self->audio_sinkpad));
  if (!audio_buffer
      &&
      !gst_aggregator_pad_is_eos (GST_AGGREGATOR_PAD_CAST
          (self->audio_sinkpad))) {
    gst_buffer_unref (video_buffer);
    return GST_AGGREGATOR_FLOW_NEED_DATA;
  }

  if (audio_buffer) {
    gst_buffer_add_video_audio_meta (video_buffer, audio_buffer);
    gst_buffer_unref (audio_buffer);
    gst_aggregator_pad_drop_buffer (GST_AGGREGATOR_PAD_CAST
        (self->audio_sinkpad));
  }
  gst_aggregator_pad_drop_buffer (GST_AGGREGATOR_PAD_CAST
      (self->video_sinkpad));

  if (!gst_pad_has_current_caps (GST_AGGREGATOR_SRC_PAD (self))) {
    GstCaps *caps = gst_caps_copy (self->video_caps);
    GstStructure *s;

    s = gst_caps_get_structure (caps, 0);
    if (self->audio_caps) {
      const GstStructure *s2;
      gint audio_channels;

      s2 = gst_caps_get_structure (self->audio_caps, 0);

      gst_structure_get_int (s2, "channels", &audio_channels);
      gst_structure_set (s, "audio-channels", G_TYPE_INT, audio_channels, NULL);
    } else {
      gst_structure_set (s, "audio-channels", G_TYPE_INT, 0, NULL);
    }

    gst_aggregator_set_src_caps (GST_AGGREGATOR (self), caps);
    gst_caps_unref (caps);
  }

  GST_AGGREGATOR_PAD (aggregator->srcpad)->segment.position =
      GST_BUFFER_PTS (video_buffer);

  return gst_aggregator_finish_buffer (GST_AGGREGATOR_CAST (self),
      video_buffer);
}

static gboolean
gst_av_combiner_sink_event (GstAggregator * aggregator,
    GstAggregatorPad * agg_pad, GstEvent * event)
{
  GstAVCombiner *self = GST_AV_COMBINER (aggregator);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:{
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);
      gst_aggregator_update_segment (GST_AGGREGATOR (self), segment);
      break;
    }
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);

      if (agg_pad == GST_AGGREGATOR_PAD_CAST (self->audio_sinkpad)) {
        if (self->audio_caps
            && !gst_caps_can_intersect (self->audio_caps, caps)) {
          GST_ERROR_OBJECT (self, "Can't update audio caps");
          return FALSE;
        }

        GST_OBJECT_LOCK (self);
        gst_caps_replace (&self->audio_caps, caps);
        GST_OBJECT_UNLOCK (self);
      } else if (agg_pad == GST_AGGREGATOR_PAD_CAST (self->video_sinkpad)) {
        if (self->video_caps
            && !gst_caps_can_intersect (self->video_caps, caps)) {
          GST_ERROR_OBJECT (self, "Can't update video caps");
          return FALSE;
        }

        GST_OBJECT_LOCK (self);
        gst_caps_replace (&self->video_caps, caps);
        GST_OBJECT_UNLOCK (self);
      }

      break;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_event (aggregator, agg_pad,
      event);
}

static gboolean
gst_av_combiner_sink_query (GstAggregator * aggregator,
    GstAggregatorPad * agg_pad, GstQuery * query)
{
  GstAVCombiner *self = GST_AV_COMBINER (aggregator);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);

      GST_OBJECT_LOCK (self);
      if (agg_pad == GST_AGGREGATOR_PAD_CAST (self->audio_sinkpad)) {
        if (self->audio_caps) {
          caps = gst_caps_ref (self->audio_caps);
          GST_OBJECT_UNLOCK (self);
        } else {
          GST_OBJECT_UNLOCK (self);
          caps = gst_pad_get_pad_template_caps (GST_PAD (agg_pad));
        }
      } else if (agg_pad == GST_AGGREGATOR_PAD_CAST (self->video_sinkpad)) {
        if (self->video_caps) {
          caps = gst_caps_ref (self->video_caps);
          GST_OBJECT_UNLOCK (self);
        } else {
          guint caps_size, i;

          GST_OBJECT_UNLOCK (self);

          caps = gst_pad_peer_query_caps (GST_AGGREGATOR_SRC_PAD (self), NULL);
          caps = gst_caps_make_writable (caps);
          caps_size = gst_caps_get_size (caps);
          for (i = 0; i < caps_size; i++) {
            GstStructure *s = gst_caps_get_structure (caps, i);
            gst_structure_remove_field (s, "audio-channels");
          }
        }
      } else {
        g_assert_not_reached ();
      }

      if (filter) {
        GstCaps *tmp = gst_caps_intersect (filter, caps);
        gst_caps_unref (caps);
        caps = tmp;
      }

      gst_query_set_caps_result (query, caps);

      return TRUE;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_query (aggregator, agg_pad,
      query);
}

static gboolean
gst_av_combiner_negotiate (GstAggregator * aggregator)
{
  return TRUE;
}

static gboolean
gst_av_combiner_stop (GstAggregator * aggregator)
{
  GstAVCombiner *self = GST_AV_COMBINER (aggregator);

  GST_OBJECT_LOCK (self);
  gst_caps_replace (&self->audio_caps, NULL);
  gst_caps_replace (&self->video_caps, NULL);
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static void
gst_av_combiner_class_init (GstAVCombinerClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstAggregatorClass *aggregator_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  aggregator_class = (GstAggregatorClass *) klass;

  gobject_class->finalize = gst_av_combiner_finalize;

  gst_element_class_set_static_metadata (gstelement_class,
      "Audio/Video Combiner",
      "Combiner",
      "Combines corresponding audio/video frames",
      "Sebastian Dröge <sebastian@centricular.com>");

  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &video_sink_template, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &audio_sink_template, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &src_template, GST_TYPE_AGGREGATOR_PAD);

  aggregator_class->aggregate = gst_av_combiner_aggregate;
  aggregator_class->stop = gst_av_combiner_stop;
  aggregator_class->sink_event = gst_av_combiner_sink_event;
  aggregator_class->sink_query = gst_av_combiner_sink_query;
  aggregator_class->negotiate = gst_av_combiner_negotiate;
  aggregator_class->get_next_time = gst_aggregator_simple_get_next_time;

  // We don't support requesting new pads
  gstelement_class->request_new_pad = NULL;

  GST_DEBUG_CATEGORY_INIT (gst_av_combiner_debug, "avcombiner",
      0, "Audio Video combiner");
}

static void
gst_av_combiner_init (GstAVCombiner * self)
{
  GstPadTemplate *templ;

  templ = gst_static_pad_template_get (&video_sink_template);
  self->video_sinkpad = GST_PAD (g_object_new (GST_TYPE_AGGREGATOR_PAD,
          "name", "video", "direction", GST_PAD_SINK, "template", templ, NULL));
  gst_object_unref (templ);
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->video_sinkpad);

  templ = gst_static_pad_template_get (&audio_sink_template);
  self->audio_sinkpad = GST_PAD (g_object_new (GST_TYPE_AGGREGATOR_PAD,
          "name", "audio", "direction", GST_PAD_SINK, "template", templ, NULL));
  gst_object_unref (templ);
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->audio_sinkpad);
}
