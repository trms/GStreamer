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

#include "gstdecklink2srcbin.h"
#include "gstdecklink2src.h"
#include "gstdecklink2utils.h"

GST_DEBUG_CATEGORY_STATIC (gst_decklink2_src_bin_debug);
#define GST_CAT_DEFAULT gst_decklink2_src_bin_debug

static GstStaticPadTemplate audio_template = GST_STATIC_PAD_TEMPLATE ("audio",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("audio/x-raw, format = (string) { S16LE, S32LE }, "
        "rate = (int) 48000, channels = (int) { 2, 8, 16 }, "
        "layout = (string) interleaved"));


struct _GstDeckLink2SrcBin
{
  GstBin parent;

  GstElement *src;
  GstElement *demux;
};

static void gst_decklink2_src_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_decklink2_src_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static void on_pad_added (GstElement * demux, GstPad * pad,
    GstDeckLink2SrcBin * self);
static void on_pad_removed (GstElement * demux, GstPad * pad,
    GstDeckLink2SrcBin * self);
static void on_no_more_pads (GstElement * demux, GstDeckLink2SrcBin * self);

#define gst_decklink2_src_bin_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2SrcBin, gst_decklink2_src_bin, GST_TYPE_BIN);
GST_ELEMENT_REGISTER_DEFINE (decklink2srcbin, "decklink2srcbin",
    GST_RANK_NONE, GST_TYPE_DECKLINK2_SRC_BIN);

static void
gst_decklink2_src_bin_class_init (GstDeckLink2SrcBinClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstCaps *templ_caps;

  object_class->set_property = gst_decklink2_src_bin_set_property;
  object_class->get_property = gst_decklink2_src_bin_get_property;

  gst_decklink2_src_install_properties (object_class);

  templ_caps = gst_decklink2_get_default_template_caps ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("video", GST_PAD_SRC, GST_PAD_ALWAYS, templ_caps));
  gst_caps_unref (templ_caps);

  gst_element_class_add_static_pad_template (element_class, &audio_template);

  gst_element_class_set_static_metadata (element_class,
      "Decklink2 Source Bin", "Video/Audio/Source/Hardware",
      "Decklink2 Source Bin", "Seungha Yang <seungha@centricular.com>");

  GST_DEBUG_CATEGORY_INIT (gst_decklink2_src_bin_debug, "decklink2srcbin",
      0, "decklink2srcbin");
}

static void
gst_decklink2_src_bin_init (GstDeckLink2SrcBin * self)
{
  GstPad *pad;
  GstPad *gpad;

  self->src = gst_element_factory_make ("decklink2src", nullptr);
  self->demux = gst_element_factory_make ("decklink2demux", nullptr);

  gst_bin_add_many (GST_BIN (self), self->src, self->demux, nullptr);
  gst_element_link (self->src, self->demux);

  pad = gst_element_get_static_pad (self->demux, "video");
  gpad = gst_ghost_pad_new ("video", pad);
  gst_object_unref (pad);
  gst_element_add_pad (GST_ELEMENT (self), gpad);

  g_signal_connect (self->demux, "pad-added", G_CALLBACK (on_pad_added), self);
  g_signal_connect (self->demux,
      "pad-removed", G_CALLBACK (on_pad_removed), self);
  g_signal_connect (self->demux, "no-more-pads",
      G_CALLBACK (on_no_more_pads), self);

  gst_bin_set_suppressed_flags (GST_BIN (self),
      (GstElementFlags) (GST_ELEMENT_FLAG_SOURCE | GST_ELEMENT_FLAG_SINK));
  GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_SOURCE);
}

static void
gst_decklink2_src_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDeckLink2SrcBin *self = GST_DECKLINK2_SRC_BIN (object);

  g_object_set_property (G_OBJECT (self->src), pspec->name, value);
}

static void
gst_decklink2_src_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDeckLink2SrcBin *self = GST_DECKLINK2_SRC_BIN (object);

  g_object_get_property (G_OBJECT (self->src), pspec->name, value);
}

static gboolean
copy_sticky_events (GstPad * pad, GstEvent ** event, GstPad * gpad)
{
  gst_pad_store_sticky_event (gpad, *event);

  return TRUE;
}

static void
on_pad_added (GstElement * demux, GstPad * pad, GstDeckLink2SrcBin * self)
{
  GstPad *gpad;
  gchar *pad_name;

  GST_DEBUG_OBJECT (self, "Pad added %" GST_PTR_FORMAT, pad);

  if (!GST_PAD_IS_SRC (pad))
    return;

  pad_name = gst_pad_get_name (pad);
  if (g_strcmp0 (pad_name, "audio") != 0) {
    g_free (pad_name);
    return;
  }

  g_free (pad_name);

  gpad = gst_ghost_pad_new ("audio", pad);
  g_object_set_data (G_OBJECT (pad), "decklink2srcbin.ghostpad", gpad);

  gst_pad_set_active (gpad, TRUE);
  gst_pad_sticky_events_foreach (pad,
      (GstPadStickyEventsForeachFunction) copy_sticky_events, gpad);
  gst_element_add_pad (GST_ELEMENT (self), gpad);
}

static void
on_pad_removed (GstElement * demux, GstPad * pad, GstDeckLink2SrcBin * self)
{
  GstPad *gpad;

  GST_DEBUG_OBJECT (self, "Pad removed %" GST_PTR_FORMAT, pad);

  if (!GST_PAD_IS_SRC (pad))
    return;

  gpad = (GstPad *) g_object_get_data (G_OBJECT (pad),
      "decklink2srcbin.ghostpad");
  if (!gpad) {
    GST_DEBUG_OBJECT (self, "No ghost pad found");
    return;
  }

  gst_element_remove_pad (GST_ELEMENT (self), gpad);
}

static void
on_no_more_pads (GstElement * demux, GstDeckLink2SrcBin * self)
{
  gst_element_no_more_pads (GST_ELEMENT (self));
}