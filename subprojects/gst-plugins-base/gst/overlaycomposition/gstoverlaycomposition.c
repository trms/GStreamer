/* GStreamer
 * Copyright (C) 2018 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2022 Seungha Yang <seungha@centricular.com>
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
 * SECTION:element-overlaycomposition
 *
 * The overlaycomposition element renders an overlay using an application
 * provided draw function and/or blends already attached
 * `GstVideoOverlayComposition` meta with incoming buffer if downstream does not
 * support the `GstVideoOverlayComposition` meta.
 *
 * ## Example code
 *
 * {{ ../../tests/examples/overlaycomposition/overlaycomposition.c[23:341] }}
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstoverlaycomposition.h"

GST_DEBUG_CATEGORY_STATIC (gst_overlay_composition_debug);
#define GST_CAT_DEFAULT gst_overlay_composition_debug

#define TEMPLATE_CAPS \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY "," \
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION, \
        GST_VIDEO_FORMATS_ALL) "; " \
    GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL) "; " \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("ANY", GST_VIDEO_FORMATS_ANY)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TEMPLATE_CAPS)
    );

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TEMPLATE_CAPS)
    );

enum
{
  SIGNAL_CAPS_CHANGED,
  SIGNAL_DRAW,
  LAST_SIGNAL
};

static guint overlay_composition_signals[LAST_SIGNAL];

typedef enum
{
  OVERLAY_MODE_UNKNOWN,
  OVERLAY_MODE_ADD_META,
  OVERLAY_MODE_BLEND,
  OVERLAY_MODE_NOT_SUPPORTED,
} OverlayMode;

struct _GstOverlayComposition
{
  GstBaseTransform parent;

  GstSample *sample;
  GstVideoInfo info;
  guint window_width;
  guint window_height;
  gboolean system_memory;
  gboolean downstream_supports_meta;
  gboolean allocation_supports_meta;
  gboolean caps_changed;
  OverlayMode overlay_mode;
};

#define gst_overlay_composition_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstOverlayComposition, gst_overlay_composition,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_overlay_composition_debug,
        "overlaycomposition", 0, "Overlay Composition"));

GST_ELEMENT_REGISTER_DEFINE (overlaycomposition, "overlaycomposition",
    GST_RANK_NONE, GST_TYPE_OVERLAY_COMPOSITION);

static gboolean gst_overlay_composition_start (GstBaseTransform * trans);
static gboolean gst_overlay_composition_stop (GstBaseTransform * trans);
static gboolean
gst_overlay_composition_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
static GstCaps *gst_overlay_composition_transform_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_overlay_composition_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_overlay_composition_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_overlay_composition_transform (GstBaseTransform *
    trans, GstBuffer * inbuf, GstBuffer * outbuf);
static GstFlowReturn gst_overlay_composition_generate_output (GstBaseTransform *
    trans, GstBuffer ** outbuf);

static void
gst_overlay_composition_class_init (GstOverlayCompositionClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  /**
   * GstOverlayComposition::draw:
   * @overlay: Overlay element emitting the signal.
   * @sample: #GstSample containing the current buffer, caps and segment.
   *
   * This signal is emitted when the overlay should be drawn.
   *
   * Returns: #GstVideoOverlayComposition or %NULL
   */
  overlay_composition_signals[SIGNAL_DRAW] =
      g_signal_new ("draw",
      G_TYPE_FROM_CLASS (klass), 0, 0, NULL, NULL, NULL,
      GST_TYPE_VIDEO_OVERLAY_COMPOSITION, 1, GST_TYPE_SAMPLE);

  /**
   * GstOverlayComposition::caps-changed:
   * @overlay: Overlay element emitting the signal.
   * @caps: The #GstCaps of the element.
   * @window_width: The window render width of downstream, or 0.
   * @window_height: The window render height of downstream, or 0.
   *
   * This signal is emitted when the caps of the element has changed.
   *
   * The window width and height define the resolution at which the frame is
   * going to be rendered in the end by e.g. a video sink (i.e. the window
   * size).
   */
  overlay_composition_signals[SIGNAL_CAPS_CHANGED] =
      g_signal_new ("caps-changed",
      G_TYPE_FROM_CLASS (klass), 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
      GST_TYPE_CAPS, G_TYPE_UINT, G_TYPE_UINT);

  gst_element_class_set_static_metadata (element_class,
      "Overlay Composition", "Filter/Editor/Video",
      "Overlay Composition", "Sebastian Dröge <sebastian@centricular.com>");

  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_add_static_pad_template (element_class, &sink_template);

  transform_class->passthrough_on_same_caps = FALSE;

  transform_class->start = GST_DEBUG_FUNCPTR (gst_overlay_composition_start);
  transform_class->stop = GST_DEBUG_FUNCPTR (gst_overlay_composition_stop);
  transform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_propose_allocation);
  transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_transform_caps);
  transform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_fixate_caps);
  transform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_set_caps);
  transform_class->transform =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_transform);
  transform_class->generate_output =
      GST_DEBUG_FUNCPTR (gst_overlay_composition_generate_output);
}

static void
gst_overlay_composition_init (GstOverlayComposition * self)
{
}

static gboolean
gst_overlay_composition_start (GstBaseTransform * trans)
{
  GstOverlayComposition *self = GST_OVERLAY_COMPOSITION (trans);

  self->sample = gst_sample_new (NULL, NULL, NULL, NULL);

  return TRUE;
}

static gboolean
gst_overlay_composition_stop (GstBaseTransform * trans)
{
  GstOverlayComposition *self = GST_OVERLAY_COMPOSITION (trans);

  g_clear_pointer (&self->sample, gst_sample_unref);

  return TRUE;
}

static gboolean
gst_overlay_composition_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
          decide_query, query)) {
    return FALSE;
  }

  /* Passthrough */
  if (!decide_query)
    return TRUE;

  return gst_pad_peer_query (trans->srcpad, query);
}

static GstCaps *
add_overlay_feature (GstCaps * caps)
{
  GstCaps *new_caps = gst_caps_new_empty ();
  guint caps_size = gst_caps_get_size (caps);
  guint i;

  for (i = 0; i < caps_size; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    GstCapsFeatures *f =
        gst_caps_features_copy (gst_caps_get_features (caps, i));
    GstCaps *c = gst_caps_new_full (gst_structure_copy (s), NULL);

    if (!gst_caps_features_is_any (f) &&
        !gst_caps_features_contains (f,
            GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION)) {
      gst_caps_features_add (f,
          GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION);
    }

    gst_caps_set_features (c, 0, f);
    gst_caps_append (new_caps, c);
  }

  return new_caps;
}

static GstCaps *
remove_overlay_feature (GstCaps * caps)
{
  GstCaps *new_caps = gst_caps_new_empty ();
  guint caps_size = gst_caps_get_size (caps);
  guint i;

  for (i = 0; i < caps_size; i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    GstCapsFeatures *f =
        gst_caps_features_copy (gst_caps_get_features (caps, i));
    GstCaps *c = gst_caps_new_full (gst_structure_copy (s), NULL);

    gst_caps_features_remove (f,
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION);
    gst_caps_set_features (c, 0, f);
    gst_caps_append (new_caps, c);
  }

  return new_caps;
}

static GstCaps *
gst_overlay_composition_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *result, *tmp;

  GST_LOG_OBJECT (trans,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  tmp = gst_caps_merge (add_overlay_feature (caps), gst_caps_ref (caps));
  tmp = gst_caps_merge (tmp, remove_overlay_feature (caps));

  if (filter) {
    GST_LOG_OBJECT (trans, "Filter caps %" GST_PTR_FORMAT, filter);

    result = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
  } else {
    result = tmp;
  }

  GST_LOG_OBJECT (trans, "returning caps: %" GST_PTR_FORMAT, result);

  return result;
}

static GstCaps *
gst_overlay_composition_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstCaps *overlay_caps = NULL;
  guint i;
  guint caps_size = gst_caps_get_size (othercaps);

  /* Prefer overlaycomposition caps */
  for (i = 0; i < caps_size; i++) {
    GstCapsFeatures *f = gst_caps_get_features (othercaps, i);

    if (f && !gst_caps_features_is_any (f) &&
        gst_caps_features_contains (f,
            GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION)) {
      GstStructure *s = gst_caps_get_structure (othercaps, i);
      overlay_caps = gst_caps_new_full (gst_structure_copy (s), NULL);
      gst_caps_set_features_simple (overlay_caps, gst_caps_features_copy (f));
      break;
    }
  }

  if (overlay_caps) {
    gst_caps_unref (othercaps);
    return gst_caps_fixate (overlay_caps);
  }

  return gst_caps_fixate (othercaps);
}

static gboolean
gst_overlay_composition_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstOverlayComposition *self = GST_OVERLAY_COMPOSITION (trans);
  GstCapsFeatures *features;

  GST_DEBUG_OBJECT (self, "Set incaps %" GST_PTR_FORMAT ", outcaps %"
      GST_PTR_FORMAT, incaps, outcaps);

  self->system_memory = FALSE;
  self->downstream_supports_meta = FALSE;

  if (!gst_video_info_from_caps (&self->info, incaps)) {
    GST_ERROR_OBJECT (self, "Invalid incaps %" GST_PTR_FORMAT, incaps);
    return FALSE;
  }

  features = gst_caps_get_features (incaps, 0);
  if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
    self->system_memory = TRUE;
  }

  features = gst_caps_get_features (outcaps, 0);
  if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION)) {
    self->downstream_supports_meta = TRUE;
  }

  gst_sample_set_caps (self->sample, incaps);
  self->caps_changed = TRUE;
  self->overlay_mode = OVERLAY_MODE_UNKNOWN;

  /* will be updated per decide-allocation  */
  self->window_width = GST_VIDEO_INFO_WIDTH (&self->info);
  self->window_height = GST_VIDEO_INFO_HEIGHT (&self->info);

  return TRUE;
}

/* Dummy implementation to make basetransform happy.
 * generate_output() method will do the actual transform */
static GstFlowReturn
gst_overlay_composition_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  return GST_FLOW_OK;
}

static const gchar *
overlay_mode_name (OverlayMode mode)
{
  switch (mode) {
    case OVERLAY_MODE_UNKNOWN:
      return "unknown";
    case OVERLAY_MODE_ADD_META:
      return "add-meta";
    case OVERLAY_MODE_BLEND:
      return "blend";
    case OVERLAY_MODE_NOT_SUPPORTED:
      return "not-supported";
  }

  return "undefined";
}

static GstFlowReturn
gst_overlay_composition_generate_output (GstBaseTransform * trans,
    GstBuffer ** outbuf)
{
  GstOverlayComposition *self = GST_OVERLAY_COMPOSITION (trans);
  GstBuffer *inbuf;
  GstVideoOverlayComposition *comp = NULL;
  GstVideoOverlayCompositionMeta *meta;
  guint i, num_rect;
  GstFlowReturn ret = GST_FLOW_OK;

  inbuf = trans->queued_buf;
  trans->queued_buf = NULL;

  if (!inbuf)
    return GST_FLOW_OK;

  if (self->overlay_mode == OVERLAY_MODE_UNKNOWN) {
    GST_DEBUG_OBJECT (self, "Deciding overlay mode, downstream-meta: %d, "
        "allocation-meta: %d, system-memory: %d",
        self->downstream_supports_meta, self->allocation_supports_meta,
        self->system_memory);

    if (self->downstream_supports_meta) {
      if (self->allocation_supports_meta || !self->system_memory) {
        self->overlay_mode = OVERLAY_MODE_ADD_META;
      } else {
        self->overlay_mode = OVERLAY_MODE_BLEND;
      }
    } else {
      if (self->system_memory) {
        self->overlay_mode = OVERLAY_MODE_BLEND;
      } else {
        /* downstream does not support meta and also not system memory.
         * Nothing we can do */
        GST_ELEMENT_WARNING (self, STREAM, NOT_IMPLEMENTED, (NULL),
            ("Neither overlay nor blending is possible"));
        self->overlay_mode = OVERLAY_MODE_NOT_SUPPORTED;
      }
    }

    GST_INFO_OBJECT (self, "Selected overlay mode: %s",
        overlay_mode_name (self->overlay_mode));
  }

  if (self->overlay_mode == OVERLAY_MODE_NOT_SUPPORTED)
    goto out;

  if (self->caps_changed) {
    g_signal_emit (self, overlay_composition_signals[SIGNAL_CAPS_CHANGED], 0,
        gst_sample_get_caps (self->sample), self->window_width,
        self->window_height, NULL);
    self->caps_changed = FALSE;
  }

  self->sample = gst_sample_make_writable (self->sample);
  gst_sample_set_buffer (self->sample, inbuf);
  gst_sample_set_segment (self->sample, &trans->segment);

  g_signal_emit (self, overlay_composition_signals[SIGNAL_DRAW], 0,
      self->sample, &comp);

  /* Don't store the buffer in the sample any longer, otherwise it will not
   * be writable below as we have one reference in the sample and one in
   * this function.
   *
   * If the sample is not writable itself then the application kept an
   * reference itself.
   */
  if (gst_sample_is_writable (self->sample))
    gst_sample_set_buffer (self->sample, NULL);

  meta = gst_buffer_get_video_overlay_composition_meta (inbuf);
  if (!comp && !meta) {
    /* Nothing to do, just forward this buffer */
    goto out;
  }

  if (self->overlay_mode == OVERLAY_MODE_ADD_META) {
    if (!comp) {
      GST_DEBUG_OBJECT (self,
          "Application did not provide an overlay composition");
      goto out;
    }

    inbuf = gst_buffer_make_writable (inbuf);
    if (!meta) {
      GST_DEBUG_OBJECT (self, "Attaching as meta");

      gst_buffer_add_video_overlay_composition_meta (inbuf, comp);
    } else {
      GST_DEBUG_OBJECT (self, "Appending to upstream overlay composition");

      meta = gst_buffer_get_video_overlay_composition_meta (inbuf);
      meta->overlay =
          gst_video_overlay_composition_make_writable (meta->overlay);

      num_rect = gst_video_overlay_composition_n_rectangles (comp);
      for (i = 0; i < num_rect; i++) {
        GstVideoOverlayRectangle *rect =
            gst_video_overlay_composition_get_rectangle (comp, i);
        gst_video_overlay_composition_add_rectangle (meta->overlay, rect);
      }
    }
  } else if (self->overlay_mode == OVERLAY_MODE_BLEND) {
    GstVideoFrame frame;

    inbuf = gst_buffer_make_writable (inbuf);
    if (!gst_video_frame_map (&frame, &self->info, inbuf,
            GST_MAP_READWRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF)) {
      GST_ERROR_OBJECT (self, "Failed to map buffer");
      ret = GST_FLOW_ERROR;
      goto out;
    }

    i = 0;
    while ((meta = gst_buffer_get_video_overlay_composition_meta (inbuf))) {
      GST_LOG_OBJECT (self, "Blending upstream overlay meta %d", i);
      gst_video_overlay_composition_blend (meta->overlay, &frame);
      gst_buffer_remove_video_overlay_composition_meta (inbuf, meta);
      i++;
    }

    if (comp) {
      GST_LOG_OBJECT (self, "Blending application overlay");
      gst_video_overlay_composition_blend (comp, &frame);
    }

    gst_video_frame_unmap (&frame);
  } else {
    g_assert_not_reached ();
  }

out:
  if (comp)
    gst_video_overlay_composition_unref (comp);

  *outbuf = inbuf;
  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (overlaycomposition, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    overlaycomposition,
    "Renders overlays on top of video frames",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
