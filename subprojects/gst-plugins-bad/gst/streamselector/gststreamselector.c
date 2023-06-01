/* GStreamer
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

/**
 * SECTION:element-streamselector
 * @title: streamselector
 *
 * Direct one out of N input streams to the output pad.
 *
 * Since: 1.24
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gststreamselector.h"

GST_DEBUG_CATEGORY_STATIC (stream_selector_debug);
#define GST_CAT_DEFAULT stream_selector_debug

enum
{
  PROP_PAD_0,
  PROP_PAD_ACTIVE,
};

struct _GstStreamSelectorPad
{
  GstAggregatorPad parent;

  GstCaps *caps;
  GstEvent *tag_event;
  gboolean active;
  gboolean discont;
};

static void gst_stream_selector_pad_dispose (GObject * object);
static void gst_stream_selector_pad_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_stream_selector_pad_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_stream_selector_pad_flush (GstAggregatorPad * pad,
    GstAggregator * agg);
static void gst_stream_selector_set_active_pad (GstStreamSelector * self,
    GstStreamSelectorPad * pad, gboolean active);

#define gst_stream_selector_pad_parent_class pad_parent_class
G_DEFINE_TYPE (GstStreamSelectorPad, gst_stream_selector_pad,
    GST_TYPE_AGGREGATOR_PAD);

static void
gst_stream_selector_pad_class_init (GstStreamSelectorPadClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstAggregatorPadClass *aggpad_class = GST_AGGREGATOR_PAD_CLASS (klass);

  object_class->dispose = gst_stream_selector_pad_dispose;
  object_class->set_property = gst_stream_selector_pad_set_property;
  object_class->get_property = gst_stream_selector_pad_get_property;

  g_object_class_install_property (object_class,
      PROP_PAD_ACTIVE, g_param_spec_boolean ("active",
          "Active", "Active state of the pad", FALSE,
          GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  aggpad_class->flush = GST_DEBUG_FUNCPTR (gst_stream_selector_pad_flush);
}

static void
gst_stream_selector_pad_init (GstStreamSelectorPad * self)
{
}

static void
gst_stream_selector_pad_dispose (GObject * object)
{
  GstStreamSelectorPad *self = GST_STREAM_SELECTOR_PAD (object);

  gst_clear_caps (&self->caps);

  G_OBJECT_CLASS (pad_parent_class)->dispose (object);
}

static void
gst_stream_selector_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorPad *pad = GST_STREAM_SELECTOR_PAD (object);
  GstStreamSelector *self = (GstStreamSelector *)
      gst_object_get_parent (GST_OBJECT_CAST (object));

  if (!self) {
    GST_WARNING_OBJECT (pad, "No parent");
    return;
  }

  switch (prop_id) {
    case PROP_PAD_ACTIVE:
      gst_stream_selector_set_active_pad (self,
          pad, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  gst_object_unref (self);
}

static void
gst_stream_selector_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorPad *pad = GST_STREAM_SELECTOR_PAD (object);

  switch (prop_id) {
    case PROP_PAD_ACTIVE:
      g_value_set_boolean (value, pad->active);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstFlowReturn
gst_stream_selector_pad_flush (GstAggregatorPad * pad, GstAggregator * agg)
{
  GstStreamSelectorPad *self = GST_STREAM_SELECTOR_PAD (pad);

  gst_clear_event (&self->tag_event);

  return GST_FLOW_OK;
}

static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

enum
{
  PROP_0,
  PROP_IGNORE_INACTIVE_PADS,
};

struct _GstStreamSelector
{
  GstAggregator parent;

  /* Current active pad, updated on aggregate() */
  GstStreamSelectorPad *active_pad;
};

static void gst_stream_selector_child_proxy_init (gpointer g_iface,
    gpointer iface_data);
static void gst_stream_selector_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_stream_selector_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static GstPad *gst_stream_selector_request_new_pad (GstElement * elem,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_stream_selector_release_pad (GstElement * elem, GstPad * pad);
static gboolean gst_stream_selector_start (GstAggregator * agg);
static gboolean gst_stream_selector_stop (GstAggregator * agg);
static gboolean gst_stream_selector_sink_query (GstAggregator * agg,
    GstAggregatorPad * pad, GstQuery * query);
static gboolean gst_stream_selector_sink_event (GstAggregator * agg,
    GstAggregatorPad * pad, GstEvent * event);
static gboolean gst_stream_selector_src_query (GstAggregator * agg,
    GstQuery * query);
static GstFlowReturn gst_stream_selector_aggregate (GstAggregator * agg,
    gboolean timeout);

#define gst_stream_selector_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstStreamSelector, gst_stream_selector,
    GST_TYPE_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_stream_selector_child_proxy_init));
GST_ELEMENT_REGISTER_DEFINE (streamselector, "streamselector",
    GST_RANK_NONE, GST_TYPE_STREAM_SELECTOR);

static void
gst_stream_selector_class_init (GstStreamSelectorClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS (klass);

  object_class->set_property = gst_stream_selector_set_property;
  object_class->get_property = gst_stream_selector_get_property;

  g_object_class_install_property (object_class,
      PROP_IGNORE_INACTIVE_PADS, g_param_spec_boolean ("ignore-inactive-pads",
          "Ignore inactive pads",
          "Avoid timing out waiting for inactive pads", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_stream_selector_request_new_pad);
  element_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_stream_selector_release_pad);

  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &sink_templ, GST_TYPE_STREAM_SELECTOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &src_templ, GST_TYPE_AGGREGATOR_PAD);

  gst_element_class_set_static_metadata (element_class, "Stream Selector",
      "Generic", "N-to-1 input stream selector",
      "Seungha Yang <seungha@centricular.com>");

  agg_class->start = GST_DEBUG_FUNCPTR (gst_stream_selector_start);
  agg_class->stop = GST_DEBUG_FUNCPTR (gst_stream_selector_stop);
  agg_class->sink_query = GST_DEBUG_FUNCPTR (gst_stream_selector_sink_query);
  agg_class->sink_event = GST_DEBUG_FUNCPTR (gst_stream_selector_sink_event);
  agg_class->src_query = GST_DEBUG_FUNCPTR (gst_stream_selector_src_query);
  agg_class->aggregate = GST_DEBUG_FUNCPTR (gst_stream_selector_aggregate);
  agg_class->get_next_time =
      GST_DEBUG_FUNCPTR (gst_aggregator_simple_get_next_time);
  agg_class->negotiate = NULL;

  gst_type_mark_as_plugin_api (GST_TYPE_STREAM_SELECTOR_PAD, 0);

  GST_DEBUG_CATEGORY_INIT (stream_selector_debug, "streamselector", 0,
      "streamselector");
}

static void
gst_stream_selector_init (GstStreamSelector * self)
{
}

static void
gst_stream_selector_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    case PROP_IGNORE_INACTIVE_PADS:
      gst_aggregator_set_ignore_inactive_pads (GST_AGGREGATOR (object),
          g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_stream_selector_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    case PROP_IGNORE_INACTIVE_PADS:
      g_value_set_boolean (value,
          gst_aggregator_get_ignore_inactive_pads (GST_AGGREGATOR (object)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GObject *
gst_stream_selector_child_proxy_get_child_by_index (GstChildProxy * proxy,
    guint index)
{
  GstElement *elem = GST_ELEMENT_CAST (proxy);
  GObject *obj = NULL;

  GST_OBJECT_LOCK (elem);
  obj = (GObject *) g_list_nth_data (elem->sinkpads, index);
  if (obj)
    gst_object_ref (obj);
  GST_OBJECT_UNLOCK (elem);

  return obj;
}

static guint
gst_stream_selector_child_proxy_get_children_count (GstChildProxy * proxy)
{
  GstElement *elem = GST_ELEMENT_CAST (proxy);
  guint count = 0;

  GST_OBJECT_LOCK (elem);
  count = elem->numsinkpads;
  GST_OBJECT_UNLOCK (elem);

  return count;
}

static void
gst_stream_selector_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = (GstChildProxyInterface *) g_iface;

  iface->get_child_by_index =
      gst_stream_selector_child_proxy_get_child_by_index;
  iface->get_children_count =
      gst_stream_selector_child_proxy_get_children_count;
}

static void
gst_stream_selector_reset (GstAggregator * agg)
{
  GstElement *elem = GST_ELEMENT_CAST (agg);
  GstStreamSelector *self = GST_STREAM_SELECTOR (agg);
  GList *iter;

  /* Clear all except for active state */
  GST_OBJECT_LOCK (self);
  for (iter = elem->sinkpads; iter; iter = g_list_next (iter)) {
    GstStreamSelectorPad *pad = GST_STREAM_SELECTOR_PAD (iter->data);
    gst_clear_caps (&pad->caps);
    gst_clear_event (&pad->tag_event);
    pad->discont = FALSE;
  }
  GST_OBJECT_UNLOCK (self);

  gst_clear_object (&self->active_pad);
}

static gboolean
gst_stream_selector_start (GstAggregator * agg)
{
  gst_stream_selector_reset (agg);

  return TRUE;
}

static gboolean
gst_stream_selector_stop (GstAggregator * agg)
{
  gst_stream_selector_reset (agg);

  return TRUE;
}

static void
gst_stream_selector_update_active_pad_unlocked (GstStreamSelector * self)
{
  GstElement *elem = GST_ELEMENT_CAST (self);
  GstPad *first = NULL;
  GstPad *active = NULL;
  GstStreamSelectorPad *spad;
  GList *iter;

  for (iter = elem->sinkpads; iter; iter = g_list_next (iter)) {
    GstPad *pad = GST_PAD_CAST (iter->data);
    spad = GST_STREAM_SELECTOR_PAD (pad);

    if (!first)
      first = pad;

    if (!active && spad->active)
      active = pad;
  }

  if (!active && first) {
    spad = GST_STREAM_SELECTOR_PAD (first);
    spad->active = TRUE;
  }
}

static void
gst_stream_selector_update_active_pad (GstStreamSelector * self)
{
  GST_OBJECT_LOCK (self);
  gst_stream_selector_update_active_pad_unlocked (self);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_stream_selector_set_active_pad (GstStreamSelector * self,
    GstStreamSelectorPad * pad, gboolean active)
{
  GstElement *elem = GST_ELEMENT_CAST (self);
  GList *iter;

  GST_OBJECT_LOCK (self);
  if (!active) {
    pad->active = FALSE;
    gst_stream_selector_update_active_pad_unlocked (self);
  } else {
    for (iter = elem->sinkpads; iter; iter = g_list_next (iter)) {
      GstStreamSelectorPad *other = GST_STREAM_SELECTOR_PAD (iter->data);
      other->active = FALSE;
    }

    pad->active = TRUE;
  }

  GST_OBJECT_UNLOCK (self);
}

static GstPad *
gst_stream_selector_get_active_pad_unlocked (GstStreamSelector * self)
{
  GstElement *elem = GST_ELEMENT_CAST (self);
  GList *iter;
  GstPad *pad = NULL;

  for (iter = elem->sinkpads; iter; iter = g_list_next (iter)) {
    GstStreamSelectorPad *spad = GST_STREAM_SELECTOR_PAD (iter->data);
    if (spad->active) {
      pad = gst_object_ref (spad);
      break;
    }
  }

  return pad;
}

static GstPad *
gst_stream_selector_get_active_pad (GstStreamSelector * self)
{
  GstPad *pad;

  GST_OBJECT_LOCK (self);
  pad = gst_stream_selector_get_active_pad_unlocked (self);
  GST_OBJECT_UNLOCK (self);

  return pad;
}

static GstPad *
gst_stream_selector_request_new_pad (GstElement * elem, GstPadTemplate * templ,
    const gchar * name, const GstCaps * caps)
{
  GstStreamSelector *self = GST_STREAM_SELECTOR (elem);
  GstPad *pad;

  pad = GST_ELEMENT_CLASS (parent_class)->request_new_pad (elem, templ, name,
      caps);
  if (!pad)
    return NULL;

  gst_stream_selector_update_active_pad (self);

  gst_child_proxy_child_added (GST_CHILD_PROXY (elem), G_OBJECT (pad),
      GST_OBJECT_NAME (pad));

  return pad;
}

static void
gst_stream_selector_release_pad (GstElement * elem, GstPad * pad)
{
  GstStreamSelector *self = GST_STREAM_SELECTOR (elem);

  gst_child_proxy_child_removed (GST_CHILD_PROXY (elem),
      G_OBJECT (pad), GST_OBJECT_NAME (pad));

  GST_ELEMENT_CLASS (parent_class)->release_pad (elem, pad);

  gst_stream_selector_update_active_pad (self);
}

static gboolean
gst_stream_selector_sink_query (GstAggregator * agg, GstAggregatorPad * pad,
    GstQuery * query)
{
  GstStreamSelector *self = GST_STREAM_SELECTOR (agg);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    {
      GstPad *active = gst_stream_selector_get_active_pad (self);
      if (!active)
        break;

      if (active == GST_PAD_CAST (pad)) {
        gboolean ret = gst_pad_query_default (active, GST_OBJECT_CAST (self),
            query);
        gst_object_unref (active);
        return ret;
      }
      gst_object_unref (active);
      return FALSE;
    }
    case GST_QUERY_CAPS:
    case GST_QUERY_ACCEPT_CAPS:
      return gst_pad_peer_query (agg->srcpad, query);
      break;
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_query (agg, pad, query);
}

static gboolean
gst_stream_selector_sink_event (GstAggregator * agg, GstAggregatorPad * pad,
    GstEvent * event)
{
  GstStreamSelector *self = GST_STREAM_SELECTOR (agg);
  GstStreamSelectorPad *spad = GST_STREAM_SELECTOR_PAD (pad);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
    {
      gst_clear_caps (&spad->caps);
      gst_clear_event (&spad->tag_event);
      break;
    }
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      GstPad *active;

      gst_event_parse_caps (event, &caps);
      gst_caps_replace (&spad->caps, caps);

      active = gst_stream_selector_get_active_pad (self);
      if (active) {
        if (active == GST_PAD_CAST (pad))
          gst_aggregator_set_src_caps (agg, caps);
        gst_object_unref (active);
      }
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      GstPad *active;
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);

      if (segment->format != GST_FORMAT_TIME) {
        GST_ERROR_OBJECT (self, "Non-TIME format segment is not supported");
        return FALSE;
      }

      active = gst_stream_selector_get_active_pad (self);
      if (!active)
        break;

      if (active == GST_PAD_CAST (pad))
        gst_aggregator_update_segment (agg, segment);

      gst_object_unref (active);
      break;
    }
    case GST_EVENT_TAG:
    {
      /* aggregator will drop tag event. Store the tag here and send later
       * on aggregate() */
      gst_clear_event (&spad->tag_event);
      spad->tag_event = event;
      return TRUE;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_event (agg, pad, event);
}

static gboolean
gst_stream_selector_src_query (GstAggregator * agg, GstQuery * query)
{
  GstStreamSelector *self = GST_STREAM_SELECTOR (agg);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstPad *active = gst_stream_selector_get_active_pad (self);
      if (active) {
        gboolean ret = gst_pad_peer_query (active, query);
        gst_object_unref (active);
        return ret;
      }
      break;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->src_query (agg, query);
}

static GstClockTime
gst_stream_selector_get_pad_running_time (GstStreamSelector * self,
    GstAggregatorPad * pad)
{
  GstClockTime running_time = GST_CLOCK_TIME_NONE;
  GstClockTime running_time_end = GST_CLOCK_TIME_NONE;
  GstClockTime timestamp;
  GstBuffer *buf;

  buf = gst_aggregator_pad_peek_buffer (pad);
  if (!buf)
    return GST_CLOCK_TIME_NONE;

  timestamp = GST_BUFFER_PTS (buf);
  if (!GST_CLOCK_TIME_IS_VALID (timestamp))
    timestamp = GST_BUFFER_DTS (buf);

  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    running_time = gst_segment_to_running_time (&pad->segment,
        GST_FORMAT_TIME, timestamp);
  }

  if (GST_CLOCK_TIME_IS_VALID (running_time)) {
    if (GST_BUFFER_DURATION_IS_VALID (buf))
      running_time_end = running_time + GST_BUFFER_DURATION (buf);
    else
      running_time_end = running_time;
  }

  gst_buffer_unref (buf);

  return running_time_end;
}

static GstFlowReturn
gst_stream_selector_aggregate (GstAggregator * agg, gboolean timeout)
{
  GstElement *elem = GST_ELEMENT_CAST (agg);
  GstStreamSelector *self = GST_STREAM_SELECTOR (agg);
  GstBuffer *buf = NULL;
  GstStreamSelectorPad *active_pad = NULL;
  GstAggregatorPad *active_agg_pad;
  GstAggregatorPad *srcpad = GST_AGGREGATOR_PAD_CAST (agg->srcpad);
  gboolean active_changed = FALSE;
  GList *iter;
  GstClockTime running_time = GST_CLOCK_TIME_NONE;
  GstClockTime running_time_end = GST_CLOCK_TIME_NONE;
  GstClockTime output_running_time = GST_CLOCK_TIME_NONE;
  GstClockTime timestamp;
  gboolean active_eos = FALSE;
  gboolean have_non_eos_pad = FALSE;

  GST_OBJECT_LOCK (self);
  active_pad = (GstStreamSelectorPad *)
      gst_stream_selector_get_active_pad_unlocked (self);
  if (!active_pad) {
    GST_WARNING_OBJECT (self, "No current active pad");
    goto need_data;
  }

  if (active_pad != self->active_pad) {
    gst_clear_object (&self->active_pad);
    self->active_pad = gst_object_ref (active_pad);
    active_changed = TRUE;
  }

  active_agg_pad = GST_AGGREGATOR_PAD_CAST (active_pad);

  buf = gst_aggregator_pad_pop_buffer (active_agg_pad);
  if (!buf) {
    if (gst_aggregator_pad_is_eos (active_agg_pad)) {
      GST_DEBUG_OBJECT (self, "Active pad is EOS");
      active_eos = TRUE;
    } else {
      GST_DEBUG_OBJECT (self, "active pad is not ready");
      goto need_data;
    }
  }

  if (buf) {
    timestamp = GST_BUFFER_PTS (buf);
    if (!GST_CLOCK_TIME_IS_VALID (timestamp))
      timestamp = GST_BUFFER_DTS (buf);

    if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
      running_time = gst_segment_to_running_time (&active_agg_pad->segment,
          GST_FORMAT_TIME, timestamp);
    }

    if (GST_CLOCK_TIME_IS_VALID (running_time)) {
      if (GST_BUFFER_DURATION_IS_VALID (buf))
        running_time_end = running_time + GST_BUFFER_DURATION (buf);
      else
        running_time_end = running_time;
    }

    GST_LOG_OBJECT (self, "Current running time %" GST_TIME_FORMAT " - %"
        GST_TIME_FORMAT, GST_TIME_ARGS (running_time),
        GST_TIME_ARGS (running_time_end));
  }

  output_running_time = gst_segment_to_running_time (&srcpad->segment,
      GST_FORMAT_TIME, srcpad->segment.position);

  for (iter = elem->sinkpads; iter; iter = g_list_next (iter)) {
    GstAggregatorPad *other_pad = GST_AGGREGATOR_PAD_CAST (iter->data);
    GstStreamSelectorPad *other_spad = GST_STREAM_SELECTOR_PAD (other_pad);
    GstClockTime other_running_time =
        gst_stream_selector_get_pad_running_time (self, other_pad);

    /* Drops other pad's buffer if
     * - active pad is eos
     * - active pad's running time is unknown
     * - or other pad's running time is unknown
     * - or active pad's running time > other_pad's running time
     */
    if (active_eos || !GST_CLOCK_TIME_IS_VALID (running_time_end) ||
        !GST_CLOCK_TIME_IS_VALID (other_running_time) ||
        (running_time_end > other_running_time)) {

      GST_LOG_OBJECT (other_pad, "Trying to drop non-active buffer, "
          "active-eos %d, active-running-time %" GST_TIME_FORMAT
          ", other-running-time %" GST_TIME_FORMAT
          ", output-running-time %" GST_TIME_FORMAT, active_eos,
          GST_TIME_ARGS (running_time_end),
          GST_TIME_ARGS (other_running_time),
          GST_TIME_ARGS (output_running_time));

      if (gst_aggregator_pad_drop_buffer (other_pad))
        other_spad->discont = TRUE;

      if (!gst_aggregator_pad_is_eos (other_pad)) {
        if (active_eos)
          GST_DEBUG_OBJECT (other_pad, "Other pad is not EOS yet");
        have_non_eos_pad = TRUE;
      }
    }
  }
  GST_OBJECT_UNLOCK (self);

  if (active_changed) {
    gst_aggregator_set_src_caps (agg, active_pad->caps);
    gst_aggregator_update_segment (agg, &active_agg_pad->segment);
  }

  if (active_pad->tag_event) {
    gst_aggregator_ensure_mandatory_events (agg);
    gst_pad_push_event (agg->srcpad, active_pad->tag_event);
    active_pad->tag_event = NULL;
  }

  if (active_eos) {
    gst_object_unref (active_pad);

    if (have_non_eos_pad) {
      GST_DEBUG_OBJECT (self,
          "Active pad is EOS, waiting for EOS from other pads");
      return GST_AGGREGATOR_FLOW_NEED_DATA;
    }

    GST_DEBUG_OBJECT (self, "All pads are EOS");

    return GST_FLOW_EOS;
  }

  srcpad->segment.position =
      gst_segment_position_from_running_time (&srcpad->segment,
      GST_FORMAT_TIME, running_time_end);

  /* Convert gap buffer to event */
  if (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_GAP) &&
      GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DROPPABLE) &&
      gst_buffer_get_size (buf) == 0) {
    GstEvent *gap;
    gst_aggregator_ensure_mandatory_events (agg);

    gap = gst_event_new_gap (GST_BUFFER_PTS (buf), GST_BUFFER_DURATION (buf));
    gst_buffer_unref (buf);

    GST_DEBUG_OBJECT (self, "Sending gap event %" GST_PTR_FORMAT, gap);
    gst_pad_push_event (agg->srcpad, gap);
    gst_object_unref (active_pad);

    return GST_FLOW_OK;
  }

  if (active_pad->discont) {
    buf = gst_buffer_make_writable (buf);
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    active_pad->discont = FALSE;
  }

  gst_object_unref (active_pad);

  return gst_aggregator_finish_buffer (agg, buf);

need_data:
  gst_clear_object (&active_pad);
  GST_OBJECT_UNLOCK (self);

  return GST_AGGREGATOR_FLOW_NEED_DATA;
}
