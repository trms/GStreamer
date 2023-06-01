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
 * SECTION:element-streamselectorbin
 * @title: streamselectorbin
 *
 * Direct one out of N input streams to the output pad.
 *
 * Since: 1.24
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gststreamselectorbin.h"
#include <gst/base/gstaggregator.h>

GST_DEBUG_CATEGORY_STATIC (stream_selector_bin_debug);
#define GST_CAT_DEFAULT stream_selector_bin_debug

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
  PROP_PAD_0,
  PROP_PAD_ACTIVE,
};

struct _GstStreamSelectorBinPad
{
  GstGhostPad parent;

  GstPad *target;
};

static void gst_stream_selector_bin_pad_dispose (GObject * object);
static void gst_stream_selector_bin_pad_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_stream_selector_bin_pad_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

#define gst_stream_selector_bin_pad_parent_class pad_parent_class
G_DEFINE_TYPE (GstStreamSelectorBinPad, gst_stream_selector_bin_pad,
    GST_TYPE_GHOST_PAD);

static void
gst_stream_selector_bin_pad_class_init (GstStreamSelectorBinPadClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_stream_selector_bin_pad_dispose;
  object_class->set_property = gst_stream_selector_bin_pad_set_property;
  object_class->get_property = gst_stream_selector_bin_pad_get_property;

  g_object_class_install_property (object_class,
      PROP_PAD_ACTIVE, g_param_spec_boolean ("active",
          "Active", "Active state of the pad", FALSE,
          GST_PARAM_MUTABLE_PLAYING | G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));
}

static void
gst_stream_selector_bin_pad_init (GstStreamSelectorBinPad * self)
{
}

static void
gst_stream_selector_bin_pad_dispose (GObject * object)
{
  GstStreamSelectorBinPad *self = GST_STREAM_SELECTOR_BIN_PAD (object);

  gst_clear_object (&self->target);

  G_OBJECT_CLASS (pad_parent_class)->dispose (object);
}

static void
gst_stream_selector_bin_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorBinPad *self = GST_STREAM_SELECTOR_BIN_PAD (object);

  if (self->target)
    g_object_set_property (G_OBJECT (self->target), pspec->name, value);
}

static void
gst_stream_selector_bin_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorBinPad *self = GST_STREAM_SELECTOR_BIN_PAD (object);

  if (self->target)
    g_object_get_property (G_OBJECT (self->target), pspec->name, value);
}

typedef enum
{
  GST_STREAM_SELECTOR_BIN_SYNC_MODE_ACTIVE_SEGMENT,
  GST_STREAM_SELECTOR_BIN_SYNC_MODE_CLOCK,
} GstStreamSelectorBinSyncMode;

#define GST_TYPE_STREAM_SELECTOR_BIN_SYNC_MODE (gst_stream_selector_bin_sync_mode_get_type())
static GType
gst_stream_selector_bin_sync_mode_get_type (void)
{
  static GType type = 0;
  static const GEnumValue sync_modes[] = {
    {GST_STREAM_SELECTOR_BIN_SYNC_MODE_ACTIVE_SEGMENT,
        "Sync using the current active segment", "active-segment"},
    {GST_STREAM_SELECTOR_BIN_SYNC_MODE_CLOCK, "Sync using the clock", "clock"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType tmp =
        g_enum_register_static ("GstStreamSelectorBinSyncMode", sync_modes);

    g_once_init_leave (&type, tmp);
  }

  return type;
}

typedef struct _GstStreamSelectorBinChain
{
  GstStreamSelectorBin *self;

  GstStreamSelectorBinPad *pad;
  GstElement *clocksync;
} GstStreamSelectorBinChain;

struct _GstStreamSelectorBin
{
  GstBin parent;

  GMutex lock;

  GstElement *selector;
  GList *input_chains;
  gboolean running;

  GstStreamSelectorBinSyncMode sync_mode;
};

enum
{
  PROP_0,
  PROP_SYNC_MODE,
  /* GstAggregator */
  PROP_LATENCY,
  PROP_MIN_UPSTREAM_LATENCY,
  PROP_START_TIME_SELECTION,
  PROP_START_TIME,
  PROP_EMIT_SIGNALS,
  /* GstStreamSelector */
  PROP_IGNORE_INACTIVE_PADS,
};

#define DEFAULT_SYNC_MODE GST_STREAM_SELECTOR_BIN_SYNC_MODE_ACTIVE_SEGMENT
#define DEFAULT_LATENCY 0
#define DEFAULT_MIN_UPSTREAM_LATENCY 0
#define DEFAULT_START_TIME_SELECTION GST_AGGREGATOR_START_TIME_SELECTION_ZERO
#define DEFAULT_START_TIME (-1)
#define DEFAULT_EMIT_SIGNALS FALSE

static void gst_stream_selector_bin_finalize (GObject * object);
static void gst_stream_selector_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_stream_selector_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static GstStateChangeReturn
gst_stream_selector_bin_change_state (GstElement * elem,
    GstStateChange transition);
static GstPad *gst_stream_selector_bin_request_new_pad (GstElement * elem,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_stream_selector_bin_release_pad (GstElement * elem,
    GstPad * pad);

#define gst_stream_selector_bin_parent_class parent_class
G_DEFINE_TYPE (GstStreamSelectorBin, gst_stream_selector_bin, GST_TYPE_BIN);
GST_ELEMENT_REGISTER_DEFINE (streamselectorbin, "streamselectorbin",
    GST_RANK_NONE, GST_TYPE_STREAM_SELECTOR_BIN);

static void
gst_stream_selector_bin_class_init (GstStreamSelectorBinClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  object_class->finalize = gst_stream_selector_bin_finalize;
  object_class->set_property = gst_stream_selector_bin_set_property;
  object_class->get_property = gst_stream_selector_bin_get_property;

  g_object_class_install_property (object_class, PROP_SYNC_MODE,
      g_param_spec_enum ("sync-mode", "Sync mode",
          "Behavior in sync-streams mode",
          GST_TYPE_STREAM_SELECTOR_BIN_SYNC_MODE,
          DEFAULT_SYNC_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* GstAggregator */
  g_object_class_install_property (object_class, PROP_LATENCY,
      g_param_spec_uint64 ("latency", "Buffer latency",
          "Additional latency in live mode to allow upstream "
          "to take longer to produce buffers for the current "
          "position (in nanoseconds)", 0, G_MAXUINT64,
          DEFAULT_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_MIN_UPSTREAM_LATENCY,
      g_param_spec_uint64 ("min-upstream-latency", "Buffer latency",
          "When sources with a higher latency are expected to be plugged "
          "in dynamically after the aggregator has started playing, "
          "this allows overriding the minimum latency reported by the "
          "initial source(s). This is only taken into account when larger "
          "than the actually reported minimum latency. (nanoseconds)",
          0, G_MAXUINT64,
          DEFAULT_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_START_TIME_SELECTION,
      g_param_spec_enum ("start-time-selection", "Start Time Selection",
          "Decides which start time is output",
          gst_aggregator_start_time_selection_get_type (),
          DEFAULT_START_TIME_SELECTION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_START_TIME,
      g_param_spec_uint64 ("start-time", "Start Time",
          "Start time to use if start-time-selection=set", 0,
          G_MAXUINT64,
          DEFAULT_START_TIME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_EMIT_SIGNALS,
      g_param_spec_boolean ("emit-signals", "Emit signals",
          "Send signals", DEFAULT_EMIT_SIGNALS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* GstStreamSelector */
  g_object_class_install_property (object_class,
      PROP_IGNORE_INACTIVE_PADS, g_param_spec_boolean ("ignore-inactive-pads",
          "Ignore inactive pads",
          "Avoid timing out waiting for inactive pads", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_stream_selector_bin_change_state);
  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_stream_selector_bin_request_new_pad);
  element_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_stream_selector_bin_release_pad);

  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &sink_templ, GST_TYPE_STREAM_SELECTOR_BIN_PAD);
  gst_element_class_add_static_pad_template (element_class, &src_templ);

  gst_element_class_set_static_metadata (element_class, "Stream Selector Bin",
      "Generic", "N-to-1 input stream selector",
      "Seungha Yang <seungha@centricular.com>");

  gst_type_mark_as_plugin_api (GST_TYPE_STREAM_SELECTOR_BIN_PAD, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_STREAM_SELECTOR_BIN_SYNC_MODE, 0);

  GST_DEBUG_CATEGORY_INIT (stream_selector_bin_debug, "streamselectorbin", 0,
      "streamselectorbin");
}

static void
gst_stream_selector_bin_init (GstStreamSelectorBin * self)
{
  GstPad *gpad;
  GstPad *srcpad;

  self->selector = gst_element_factory_make ("streamselector",
      "stream-selector");

  gst_bin_add (GST_BIN_CAST (self), self->selector);

  srcpad = gst_element_get_static_pad (GST_ELEMENT_CAST (self->selector),
      "src");
  gpad = gst_ghost_pad_new ("src", srcpad);
  gst_object_unref (srcpad);

  gst_element_add_pad (GST_ELEMENT_CAST (self), gpad);

  self->sync_mode = DEFAULT_SYNC_MODE;

  g_mutex_init (&self->lock);
}

static void
gst_stream_selector_bin_finalize (GObject * object)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (object);

  if (self->input_chains)
    g_list_free_full (self->input_chains, (GDestroyNotify) g_free);

  g_mutex_clear (&self->lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_stream_selector_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (object);

  switch (prop_id) {
    case PROP_SYNC_MODE:
    {
      GList *iter;
      gboolean sync = FALSE;
      g_mutex_lock (&self->lock);
      self->sync_mode = g_value_get_enum (value);
      if (self->sync_mode == GST_STREAM_SELECTOR_BIN_SYNC_MODE_CLOCK)
        sync = TRUE;

      for (iter = self->input_chains; iter; iter = g_list_next (iter)) {
        GstStreamSelectorBinChain *chain = iter->data;
        if (chain->clocksync)
          g_object_set (chain->clocksync, "sync", sync, NULL);
      }
      g_mutex_unlock (&self->lock);
      break;
    }
    default:
      g_object_set_property (G_OBJECT (self->selector), pspec->name, value);
      break;
  }
}

static void
gst_stream_selector_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (object);

  switch (prop_id) {
    case PROP_SYNC_MODE:
      g_mutex_lock (&self->lock);
      g_value_set_enum (value, self->sync_mode);
      g_mutex_unlock (&self->lock);
      break;
    default:
      g_object_get_property (G_OBJECT (self->selector), pspec->name, value);
      break;
  }
}

static GstStateChangeReturn
gst_stream_selector_bin_change_state (GstElement * elem,
    GstStateChange transition)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (elem);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      g_mutex_lock (&self->lock);
      self->running = TRUE;
      g_mutex_unlock (&self->lock);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (elem, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_mutex_lock (&self->lock);
      self->running = FALSE;
      g_mutex_unlock (&self->lock);
    default:
      break;
  }

  return ret;
}

static GstStreamSelectorBinChain *
gst_stream_selector_bin_chain_new (GstStreamSelectorBin * self,
    GstPad * selector_pad)
{
  GstStreamSelectorBinChain *chain;
  GstStreamSelectorBinPad *binpad;
  GstPad *pad;

  chain = g_new0 (GstStreamSelectorBinChain, 1);
  chain->self = self;
  chain->clocksync = gst_element_factory_make ("clocksync", NULL);

  gst_bin_add (GST_BIN_CAST (self), chain->clocksync);

  pad = gst_element_get_static_pad (chain->clocksync, "src");
  gst_pad_link (pad, selector_pad);
  gst_object_unref (pad);

  chain->pad = g_object_new (GST_TYPE_STREAM_SELECTOR_BIN_PAD,
      "name", GST_OBJECT_NAME (selector_pad), "direction", GST_PAD_SINK, NULL);

  binpad = GST_STREAM_SELECTOR_BIN_PAD (chain->pad);
  binpad->target = selector_pad;

  pad = gst_element_get_static_pad (chain->clocksync, "sink");
  gst_ghost_pad_set_target (GST_GHOST_PAD_CAST (chain->pad), pad);
  gst_object_unref (pad);

  g_mutex_lock (&self->lock);
  if (self->running)
    gst_pad_set_active (GST_PAD_CAST (chain->pad), TRUE);
  g_mutex_unlock (&self->lock);

  gst_element_add_pad (GST_ELEMENT_CAST (self), GST_PAD_CAST (chain->pad));
  gst_element_sync_state_with_parent (chain->clocksync);

  return chain;
}

static void
gst_stream_selector_bin_chain_free (GstStreamSelectorBinChain * chain)
{
  GstStreamSelectorBin *self = chain->self;

  gst_element_set_locked_state (chain->clocksync, TRUE);
  gst_element_set_state (chain->clocksync, GST_STATE_NULL);
  gst_bin_remove (GST_BIN_CAST (chain->self), chain->clocksync);

  gst_element_release_request_pad (self->selector, chain->pad->target);
  gst_clear_object (&chain->pad->target);

  g_free (chain);
}

static GstPad *
gst_stream_selector_bin_request_new_pad (GstElement * elem,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (elem);
  GstPad *selector_pad;
  GstStreamSelectorBinChain *chain;
  gboolean sync = FALSE;
  GstPadTemplate *selector_templ;

  selector_templ = gst_element_get_pad_template (self->selector, "sink_%u");
  selector_pad = gst_element_request_pad (self->selector, selector_templ, name, caps);
  if (!selector_pad)
    return NULL;

  chain = gst_stream_selector_bin_chain_new (self, selector_pad);

  g_mutex_lock (&self->lock);
  if (self->sync_mode == GST_STREAM_SELECTOR_BIN_SYNC_MODE_CLOCK)
    sync = TRUE;
  g_object_set (chain->clocksync, "sync", sync, NULL);
  self->input_chains = g_list_append (self->input_chains, chain);
  g_mutex_unlock (&self->lock);

  GST_DEBUG_OBJECT (chain->pad, "Created new pad");

  return GST_PAD_CAST (chain->pad);
}

static void
gst_stream_selector_bin_release_pad (GstElement * elem, GstPad * pad)
{
  GstStreamSelectorBin *self = GST_STREAM_SELECTOR_BIN (elem);
  GList *iter;
  gboolean found = FALSE;

  g_mutex_lock (&self->lock);
  for (iter = self->input_chains; iter; iter = g_list_next (iter)) {
    GstStreamSelectorBinChain *chain = iter->data;

    if (pad == GST_PAD_CAST (chain->pad)) {
      self->input_chains = g_list_delete_link (self->input_chains, iter);
      g_mutex_unlock (&self->lock);

      gst_stream_selector_bin_chain_free (chain);
      found = TRUE;
      break;
    }
  }

  if (!found) {
    g_mutex_unlock (&self->lock);
    GST_WARNING_OBJECT (self, "Unknown pad to release %s:%s",
        GST_DEBUG_PAD_NAME (pad));
  }

  gst_element_remove_pad (elem, pad);
}
