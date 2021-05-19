/* GStreamer
 *
 * Copyright (C) 2011 David Schleef <ds@schleef.org>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_DECKLINK_SINK_H__
#define __GST_DECKLINK_SINK_H__

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/video/video.h>
#include "gstdecklink.h"

G_BEGIN_DECLS

#define GST_TYPE_DECKLINK_SINK \
  (gst_decklink_sink_get_type())
#define GST_DECKLINK_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DECKLINK_SINK, GstDecklinkSink))
#define GST_DECKLINK_SINK_CAST(obj) \
  ((GstDecklinkSink*)obj)
#define GST_DECKLINK_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DECKLINK_SINK, GstDecklinkSinkClass))
#define GST_IS_DECKLINK_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DECKLINK_SINK))
#define GST_IS_DECKLINK_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DECKLINK_SINK))

typedef struct _GstDecklinkSink GstDecklinkSink;
typedef struct _GstDecklinkSinkClass GstDecklinkSinkClass;

struct _GstDecklinkSink
{
  GstBaseSink parent;

  GstDecklinkModeEnum mode;
  gint device_number;
  GstDecklinkVideoFormat video_format;
  BMDTimecodeFormat timecode_format;
  BMDKeyerMode keyer_mode;
  gint keyer_level;
  gint caption_line;
  gint afd_bar_line;
  gint n_preroll_frames;
  gint min_buffered_frames;
  gint max_buffered_frames;

  GstDecklinkOutput *output;

  GstVideoInfo info;
  gint audio_channels;

  GMutex schedule_lock;
  GstBuffer *last_buffer;
  guint n_frames;

  gint n_prerolled_frames;

  GstVideoVBIEncoder *vbiencoder;
  GstVideoFormat anc_vformat;
  guint16 cdp_hdr_sequence_cntr;
};

struct _GstDecklinkSinkClass
{
  GstBaseSinkClass parent_class;
};

GType gst_decklink_sink_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (decklinksink);

G_END_DECLS

#endif /* __GST_DECKLINK_SINK_H__ */
