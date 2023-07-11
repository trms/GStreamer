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

#include "gstdecklink2combiner.h"
#include "gstdecklink2utils.h"

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_combiner_debug);
#define GST_CAT_DEFAULT gst_decklink2_combiner_debug

static GstStaticPadTemplate audio_template = GST_STATIC_PAD_TEMPLATE ("audio",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, format = (string) { S16LE, S32LE }, "
        "rate = (int) 48000, channels = (int) { 2, 8, 16 }, "
        "layout = (string) interleaved"));

struct _GstDeckLink2Combiner
{
  GstAggregator parent;

  GstAggregatorPad *video_pad;
  GstAggregatorPad *audio_pad;

  GstCaps *video_caps;
  GstCaps *audio_caps;

  GstVideoInfo video_info;
  GstAudioInfo audio_info;

  GstAdapter *audio_buffers;

  GstClockTime video_start_time;
  GstClockTime audio_start_time;

  GstClockTime video_running_time;
  GstClockTime audio_running_time;

  guint64 num_video_buffers;
  guint64 num_audio_buffers;
};

static void gst_decklink2_combiner_dispose (GObject * object);
static gboolean gst_decklink2_combiner_sink_event (GstAggregator * agg,
    GstAggregatorPad * pad, GstEvent * event);
static gboolean gst_decklink2_combiner_sink_query (GstAggregator * agg,
    GstAggregatorPad * aggpad, GstQuery * query);
static GstFlowReturn gst_decklink2_combiner_aggregate (GstAggregator * agg,
    gboolean timeout);
static gboolean gst_decklink2_combiner_start (GstAggregator * agg);
static gboolean gst_decklink2_combiner_stop (GstAggregator * agg);
static GstBuffer *gst_decklink2_combiner_clip (GstAggregator * agg,
    GstAggregatorPad * aggpad, GstBuffer * buffer);

#define gst_decklink2_combiner_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Combiner, gst_decklink2_combiner,
    GST_TYPE_AGGREGATOR);
GST_ELEMENT_REGISTER_DEFINE (decklink2combiner, "decklink2combiner",
    GST_RANK_NONE, GST_TYPE_DECKLINK2_COMBINER);

static void
gst_decklink2_combiner_class_init (GstDeckLink2CombinerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS (klass);
  GstCaps *templ_caps;

  object_class->dispose = gst_decklink2_combiner_dispose;

  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &audio_template, GST_TYPE_AGGREGATOR_PAD);

  templ_caps = gst_decklink2_get_default_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new_with_gtype ("video", GST_PAD_SINK, GST_PAD_ALWAYS,
          templ_caps, GST_TYPE_AGGREGATOR_PAD));

  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new_with_gtype ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          templ_caps, GST_TYPE_AGGREGATOR_PAD));
  gst_caps_unref (templ_caps);

  gst_element_class_set_static_metadata (element_class,
      "DeckLink2 Combiner",
      "Combiner", "Combines video and audio frames",
      "Seungha Yang <seungha@centricular.com>");

  agg_class->sink_event = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_sink_event);
  agg_class->sink_query = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_sink_query);
  agg_class->aggregate = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_aggregate);
  agg_class->start = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_start);
  agg_class->stop = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_stop);
  agg_class->clip = GST_DEBUG_FUNCPTR (gst_decklink2_combiner_clip);
  agg_class->get_next_time =
      GST_DEBUG_FUNCPTR (gst_aggregator_simple_get_next_time);
  /* No negotiation needed */
  agg_class->negotiate = NULL;

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_combiner_debug,
      "decklink2combiner", 0, "decklink2combiner");
}

static void
gst_decklink2_combiner_init (GstDeckLink2Combiner * self)
{
  GstPadTemplate *templ;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (self);

  templ = gst_element_class_get_pad_template (klass, "video");
  self->video_pad = (GstAggregatorPad *)
      g_object_new (GST_TYPE_AGGREGATOR_PAD, "name", "video", "direction",
      GST_PAD_SINK, "template", templ, NULL);
  gst_object_unref (templ);
  gst_element_add_pad (GST_ELEMENT_CAST (self), GST_PAD_CAST (self->video_pad));

  templ = gst_static_pad_template_get (&audio_template);
  self->audio_pad = (GstAggregatorPad *)
      g_object_new (GST_TYPE_AGGREGATOR_PAD, "name", "audio", "direction",
      GST_PAD_SINK, "template", templ, NULL);
  gst_object_unref (templ);
  gst_element_add_pad (GST_ELEMENT_CAST (self), GST_PAD_CAST (self->audio_pad));

  self->audio_buffers = gst_adapter_new ();
}

static void
gst_decklink2_combiner_dispose (GObject * object)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (object);

  g_clear_object (&self->audio_buffers);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static gboolean
gst_decklink2_combiner_sink_event (GstAggregator * agg,
    GstAggregatorPad * aggpad, GstEvent * event)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);

      GST_DEBUG_OBJECT (self, "Got caps from %s pad %" GST_PTR_FORMAT,
          aggpad == self->video_pad ? "video" : "audio", caps);

      if (aggpad == self->video_pad) {
        gst_caps_replace (&self->video_caps, caps);
        gst_video_info_from_caps (&self->video_info, caps);
      } else {
        /* FIXME: flush audio if audio info is changed or disallow audio update */
        gst_caps_replace (&self->audio_caps, caps);
        gst_audio_info_from_caps (&self->audio_info, caps);
      }

      if (self->video_caps) {
        gint fps_n, fps_d;
        GstClockTime latency;

        caps = gst_caps_copy (self->video_caps);
        if (GST_AUDIO_INFO_IS_VALID (&self->audio_info)) {
          gst_caps_set_simple (caps, "audio-channels", G_TYPE_INT,
              self->audio_info.channels,
              "audio-format", G_TYPE_STRING,
              gst_audio_format_to_string (self->audio_info.finfo->format),
              NULL);
        } else {
          gst_caps_set_simple (caps, "audio-channels", G_TYPE_INT, 0, NULL);
        }

        GST_DEBUG_OBJECT (self, "Set caps %" GST_PTR_FORMAT, caps);

        if (self->video_info.fps_n > 0 && self->video_info.fps_d > 0) {
          fps_n = self->video_info.fps_n;
          fps_d = self->video_info.fps_d;
        } else {
          fps_n = 30;
          fps_d = 1;
        }

        latency = gst_util_uint64_scale (GST_SECOND, fps_d, fps_n);
        gst_aggregator_set_latency (agg, latency, latency);

        gst_aggregator_set_src_caps (agg, caps);
        gst_caps_unref (caps);
      }
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);

      /* pass through video segment as-is */
      gst_aggregator_update_segment (agg, segment);
      break;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_event (agg, aggpad, event);
}

static gboolean
gst_decklink2_combiner_sink_query (GstAggregator * agg,
    GstAggregatorPad * aggpad, GstQuery * query)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);
  gboolean ret;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstQuery *caps_query;
      GstCaps *filter = NULL;
      GstStructure *s;
      GstCaps *caps = NULL;
      GstCaps *templ_caps = gst_pad_get_pad_template_caps (GST_PAD (aggpad));

      gst_query_parse_caps (query, &filter);

      GST_LOG_OBJECT (aggpad, "Handle query caps with filter %" GST_PTR_FORMAT,
          filter);

      if (filter)
        caps_query = gst_query_new_caps (filter);
      else
        caps_query = gst_query_new_caps (templ_caps);

      ret = gst_pad_peer_query (GST_AGGREGATOR_SRC_PAD (agg), caps_query);
      gst_query_parse_caps_result (caps_query, &caps);

      GST_LOG_OBJECT (aggpad, "Downstream query caps result %d, %"
          GST_PTR_FORMAT, ret, caps);

      if (!caps || gst_caps_is_empty (caps) || gst_caps_is_any (caps)) {
        if (filter) {
          GstCaps *temp = gst_caps_intersect_full (filter, templ_caps,
              GST_CAPS_INTERSECT_FIRST);
          gst_query_set_caps_result (query, temp);
          gst_caps_unref (temp);
        } else {
          gst_query_set_caps_result (query, templ_caps);
        }
        gst_caps_unref (templ_caps);
        gst_query_unref (caps_query);

        return TRUE;
      }

      caps = gst_caps_copy (caps);
      gst_query_unref (caps_query);

      if (aggpad == self->video_pad) {
        /* Remove audio related fields */
        for (guint i = 0; i < gst_caps_get_size (caps); i++) {
          s = gst_caps_get_structure (caps, i);
          gst_structure_remove_field (s, "audio-channels");
        }
        gst_query_set_caps_result (query, caps);
        gst_caps_unref (caps);
      } else {
        GstCaps *audio_caps = gst_caps_copy (templ_caps);
        const GValue *ch;

        /* construct caps with updated channels field */
        s = gst_caps_get_structure (caps, 0);
        ch = gst_structure_get_value (s, "audio-channels");
        if (ch)
          gst_caps_set_value (audio_caps, "channels", ch);

        gst_caps_unref (caps);

        if (filter) {
          GstCaps *temp = gst_caps_intersect_full (filter, audio_caps,
              GST_CAPS_INTERSECT_FIRST);
          gst_query_set_caps_result (query, temp);
          gst_caps_unref (temp);
        } else {
          gst_query_set_caps_result (query, audio_caps);
        }
        gst_caps_unref (audio_caps);
      }
      gst_caps_unref (templ_caps);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:
      GST_DEBUG_OBJECT (aggpad, "Handle accept caps");

      if (aggpad == self->video_pad) {
        ret = gst_pad_peer_query (GST_AGGREGATOR_SRC_PAD (agg), query);
        GST_DEBUG_OBJECT (aggpad, "Video accept caps result %d", ret);
      } else {
        GstQuery *caps_query;
        GstCaps *audio_caps;
        GstCaps *caps = NULL;
        const GValue *ch;
        GstStructure *s;

        caps_query = gst_query_new_caps (NULL);
        ret = gst_pad_peer_query (GST_AGGREGATOR_SRC_PAD (agg), caps_query);

        gst_query_parse_caps_result (caps_query, &caps);
        GST_LOG_OBJECT (aggpad, "Downstream query caps result %d, %"
            GST_PTR_FORMAT, ret, caps);

        if (!caps || gst_caps_is_empty (caps) || gst_caps_is_any (caps)) {
          gst_query_unref (caps_query);
          gst_query_set_accept_caps_result (query, TRUE);

          return TRUE;
        }

        audio_caps = gst_static_pad_template_get_caps (&audio_template);
        /* construct caps with updated channels field */
        audio_caps = gst_caps_copy (audio_caps);

        s = gst_caps_get_structure (caps, 0);
        ch = gst_structure_get_value (s, "audio-channels");
        if (ch)
          gst_caps_set_value (audio_caps, "channels", ch);

        gst_query_unref (caps_query);

        gst_query_parse_accept_caps (query, &caps);
        gst_query_set_accept_caps_result (query, gst_caps_is_subset (caps,
                audio_caps));
        gst_caps_unref (audio_caps);
        ret = TRUE;
      }
      return ret;
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_query (agg, aggpad, query);
}

static void
gst_decklink2_combiner_reset (GstDeckLink2Combiner * self)
{
  gst_clear_caps (&self->video_caps);
  gst_clear_caps (&self->audio_caps);

  gst_adapter_clear (self->audio_buffers);

  gst_video_info_init (&self->video_info);
  gst_audio_info_init (&self->audio_info);

  self->video_start_time = GST_CLOCK_TIME_NONE;
  self->audio_start_time = GST_CLOCK_TIME_NONE;
  self->video_running_time = GST_CLOCK_TIME_NONE;
  self->audio_running_time = GST_CLOCK_TIME_NONE;
  self->num_video_buffers = 0;
  self->num_audio_buffers = 0;
}

static gboolean
gst_decklink2_combiner_start (GstAggregator * agg)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);

  gst_decklink2_combiner_reset (self);

  return TRUE;
}

static gboolean
gst_decklink2_combiner_stop (GstAggregator * agg)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);

  gst_decklink2_combiner_reset (self);

  return TRUE;
}

static GstBuffer *
gst_decklink2_combiner_clip (GstAggregator * agg, GstAggregatorPad * aggpad,
    GstBuffer * buffer)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);
  GstClockTime pts;

  pts = GST_BUFFER_PTS (buffer);

  if (!GST_CLOCK_TIME_IS_VALID (pts)) {
    GST_ERROR_OBJECT (self, "Only buffers with PTS supported");
    return buffer;
  }

  if (aggpad == self->video_pad) {
    GstClockTime dur;
    GstClockTime start, stop, cstart, cstop;

    dur = GST_BUFFER_DURATION (buffer);
    if (!GST_CLOCK_TIME_IS_VALID (dur) &&
        self->video_info.fps_n > 0 && self->video_info.fps_d > 0) {
      dur = gst_util_uint64_scale_int (GST_SECOND, self->video_info.fps_d,
          self->video_info.fps_n);
    }

    start = pts;
    if (GST_CLOCK_TIME_IS_VALID (dur))
      stop = start + dur;
    else
      stop = GST_CLOCK_TIME_NONE;

    if (!gst_segment_clip (&aggpad->segment, GST_FORMAT_TIME, start, stop,
            &cstart, &cstop)) {
      GST_LOG_OBJECT (self, "Dropping buffer outside segment");
      gst_buffer_unref (buffer);
      return NULL;
    }

    if (GST_BUFFER_PTS (buffer) != cstart) {
      buffer = gst_buffer_make_writable (buffer);
      GST_BUFFER_PTS (buffer) = cstart;
    }

    if (GST_CLOCK_TIME_IS_VALID (stop) && GST_CLOCK_TIME_IS_VALID (cstop)) {
      dur = cstop - cstart;

      if (GST_BUFFER_DURATION (buffer) != dur)
        buffer = gst_buffer_make_writable (buffer);

      GST_BUFFER_DURATION (buffer) = dur;
    }
  } else {
    buffer = gst_audio_buffer_clip (buffer, &aggpad->segment,
        self->audio_info.rate, self->audio_info.bpf);
  }

  return buffer;
}

static GstFlowReturn
gst_decklink2_combiner_aggregate (GstAggregator * agg, gboolean timeout)
{
  GstDeckLink2Combiner *self = GST_DECKLINK2_COMBINER (agg);
  GstBuffer *video_buf = NULL;
  GstBuffer *audio_buf = NULL;
  gsize audio_buf_size;
  GstDeckLink2AudioMeta *meta;
  GstClockTime video_running_time = GST_CLOCK_TIME_NONE;
  GstClockTime video_running_time_end = GST_CLOCK_TIME_NONE;

  video_buf = gst_aggregator_pad_peek_buffer (self->video_pad);
  if (!video_buf) {
    if (gst_aggregator_pad_is_eos (self->video_pad)) {
      /* Follow video stream's timeline */
      GST_DEBUG_OBJECT (self, "Video pad is EOS");
      return GST_FLOW_EOS;
    }

    /* Need to know video start time */
    if (!GST_CLOCK_TIME_IS_VALID (self->video_start_time)) {
      GST_LOG_OBJECT (self, "Waiting for first video buffer");
      goto again;
    }

    GST_LOG_OBJECT (self, "Video is not ready");
  } else {
    /* Drop empty buffer */
    if (gst_buffer_get_size (video_buf) == 0) {
      GST_LOG_OBJECT (self, "Dropping empty video buffer");
      gst_aggregator_pad_drop_buffer (self->video_pad);
      goto again;
    }

    video_running_time = video_running_time_end =
        gst_segment_to_running_time (&self->video_pad->segment,
        GST_FORMAT_TIME, GST_BUFFER_PTS (video_buf));
    if (GST_BUFFER_DURATION_IS_VALID (video_buf)) {
      video_running_time_end += GST_BUFFER_DURATION (video_buf);
    } else if (self->video_info.fps_n > 0 && self->video_info.fps_d > 0) {
      video_running_time_end += gst_util_uint64_scale_int (GST_SECOND,
          self->video_info.fps_d, self->video_info.fps_n);
    } else {
      /* XXX: shouldn't happen */
      video_running_time_end = video_running_time;
    }

    if (!GST_CLOCK_TIME_IS_VALID (self->video_start_time)) {
      self->video_start_time = video_running_time;
      GST_DEBUG_OBJECT (self, "Video start time %" GST_TIME_FORMAT,
          GST_TIME_ARGS (self->video_start_time));
    }

    self->video_running_time = video_running_time_end;
  }

  audio_buf = gst_aggregator_pad_peek_buffer (self->audio_pad);
  if (!audio_buf) {
    if (gst_adapter_available (self->audio_buffers) == 0 &&
        !gst_aggregator_pad_is_eos (self->audio_pad) &&
        self->audio_running_time < self->video_running_time) {
      GST_LOG_OBJECT (self, "Waiting for audio buffer");
      goto again;
    }
  } else if (gst_buffer_get_size (audio_buf) == 0) {
    GST_LOG_OBJECT (self, "Dropping empty audio buffer");
    gst_aggregator_pad_drop_buffer (self->audio_pad);
    goto again;
  } else {
    GstClockTime audio_running_time, audio_running_time_end;

    audio_running_time = gst_segment_to_running_time (&self->audio_pad->segment,
        GST_FORMAT_TIME, GST_BUFFER_PTS (audio_buf));
    if (GST_BUFFER_DURATION_IS_VALID (audio_buf)) {
      audio_running_time_end = audio_running_time +
          GST_BUFFER_DURATION (audio_buf);
    } else {
      audio_running_time_end = gst_util_uint64_scale (GST_SECOND,
          gst_buffer_get_size (audio_buf),
          self->audio_info.rate * self->audio_info.bpf);
      audio_running_time_end += audio_running_time;
    }

    self->audio_running_time = audio_running_time_end;

    /* Do initial video/audio align */
    if (!GST_CLOCK_TIME_IS_VALID (self->audio_start_time)) {
      GST_DEBUG_OBJECT (self, "Initial audio running time %" GST_TIME_FORMAT,
          GST_TIME_ARGS (audio_running_time));

      if (audio_running_time_end <= self->video_start_time) {
        GST_DEBUG_OBJECT (self, "audio running-time end %" GST_TIME_FORMAT
            " < video-start-time %" GST_TIME_FORMAT,
            GST_TIME_ARGS (self->video_start_time),
            GST_TIME_ARGS (audio_running_time_end));
        /* completely outside */
        gst_aggregator_pad_drop_buffer (self->audio_pad);
        goto again;
      } else if (audio_running_time < self->video_start_time &&
          audio_running_time_end >= self->video_start_time) {
        /* partial overlap */
        GstClockTime diff;
        gsize in_samples, diff_samples;
        GstAudioMeta *meta;
        GstBuffer *trunc_buf;

        meta = gst_buffer_get_audio_meta (audio_buf);
        in_samples = meta ? meta->samples :
            gst_buffer_get_size (audio_buf) / self->audio_info.bpf;

        diff = self->video_start_time - audio_running_time;
        diff_samples = gst_util_uint64_scale (diff,
            self->audio_info.rate, GST_SECOND);

        GST_DEBUG_OBJECT (self, "Truncate initial audio buffer duration %"
            GST_TIME_FORMAT, GST_TIME_ARGS (diff));

        trunc_buf = gst_audio_buffer_truncate (
            (GstBuffer *) g_steal_pointer (&audio_buf),
            self->audio_info.bpf, diff_samples, in_samples - diff_samples);
        gst_aggregator_pad_drop_buffer (self->audio_pad);
        if (!trunc_buf) {
          GST_DEBUG_OBJECT (self, "Empty truncated buffer");
          gst_aggregator_pad_drop_buffer (self->audio_pad);
          goto again;
        }

        self->audio_start_time = self->video_start_time;
        gst_adapter_push (self->audio_buffers, trunc_buf);
      } else if (audio_running_time >= self->video_start_time) {
        /* fill silence if needed */
        GstClockTime diff;
        gsize diff_samples;

        diff = audio_running_time - self->video_start_time;
        if (diff > 0) {
          gsize fill_size;

          diff_samples = gst_util_uint64_scale (diff,
              self->audio_info.rate, GST_SECOND);

          fill_size = diff_samples * self->audio_info.bpf;
          if (fill_size > 0) {
            GstBuffer *fill_buf;
            GstMapInfo map;

            GST_DEBUG_OBJECT (self, "Fill initial %" G_GSIZE_FORMAT
                " audio samples", diff_samples);

            fill_buf = gst_buffer_new_and_alloc (fill_size);
            gst_buffer_map (fill_buf, &map, GST_MAP_WRITE);
            gst_audio_format_info_fill_silence (self->audio_info.finfo,
                map.data, map.size);
            gst_buffer_unmap (fill_buf, &map);
            gst_adapter_push (self->audio_buffers, fill_buf);
          }
        }

        self->audio_start_time = self->video_start_time;

        gst_adapter_push (self->audio_buffers,
            (GstBuffer *) g_steal_pointer (&audio_buf));
        gst_aggregator_pad_drop_buffer (self->audio_pad);
      }

      self->num_audio_buffers++;
    } else {
      GST_LOG_OBJECT (self, "Pushing audio buffer to adapter, %" GST_PTR_FORMAT,
          audio_buf);
      gst_adapter_push (self->audio_buffers,
          (GstBuffer *) g_steal_pointer (&audio_buf));
      gst_aggregator_pad_drop_buffer (self->audio_pad);

      self->num_audio_buffers++;
    }
  }

  if (!video_buf) {
    GST_LOG_OBJECT (self, "Waiting for video");
    goto again;
  } else if (!gst_aggregator_pad_is_eos (self->audio_pad) &&
      self->audio_running_time < self->video_running_time) {
    GST_LOG_OBJECT (self, "Waiting for audio, audio running time %"
        GST_TIME_FORMAT " < video running time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->audio_running_time),
        GST_TIME_ARGS (self->video_running_time));
    goto again;
  }

  gst_aggregator_pad_drop_buffer (self->video_pad);
  video_buf = gst_buffer_make_writable (video_buf);
  self->num_video_buffers++;

  /* Remove external audio meta if any */
  meta = gst_buffer_get_decklink2_audio_meta (video_buf);
  if (meta) {
    GST_LOG_OBJECT (self, "Removing old audio meta");
    gst_buffer_remove_meta (video_buf, GST_META_CAST (meta));
  }

  audio_buf_size = gst_adapter_available (self->audio_buffers);
  if (audio_buf_size > 0) {
    GstSample *audio_sample;

    audio_buf = gst_adapter_take_buffer (self->audio_buffers, audio_buf_size);
    audio_sample = gst_sample_new (audio_buf, self->audio_caps, NULL, NULL);

    GST_LOG_OBJECT (self, "Adding meta with size %" G_GSIZE_FORMAT,
        gst_buffer_get_size (audio_buf));
    gst_buffer_unref (audio_buf);

    gst_buffer_add_decklink2_audio_meta (video_buf, audio_sample);
    gst_sample_unref (audio_sample);
  } else {
    GST_LOG_OBJECT (self, "No audio meta");
  }

  GST_LOG_OBJECT (self, "Finish buffer %" GST_PTR_FORMAT
      ", total video/audio buffers %" GST_TIME_FORMAT
      " (%" G_GUINT64_FORMAT ") / %" GST_TIME_FORMAT " (%"
      G_GUINT64_FORMAT ")", video_buf, GST_TIME_ARGS (self->video_running_time),
      self->num_video_buffers, GST_TIME_ARGS (self->audio_running_time),
      self->num_audio_buffers);

  GST_AGGREGATOR_PAD (agg->srcpad)->segment.position = self->video_running_time;

  return gst_aggregator_finish_buffer (agg, video_buf);

again:
  gst_clear_buffer (&video_buf);
  gst_clear_buffer (&audio_buf);

  return GST_AGGREGATOR_FLOW_NEED_DATA;
}
