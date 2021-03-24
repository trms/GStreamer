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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_AV_COMBINER_H_
#define _GST_AV_COMBINER_H_

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_AV_COMBINER \
  (gst_av_combiner_get_type())
#define GST_AV_COMBINER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AV_COMBINER,GstAVCombiner))
#define GST_AV_COMBINER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AV_COMBINER,GstAVCombinerClass))
#define IS_GST_AV_COMBINER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AV_COMBINER))
#define IS_GST_AV_COMBINER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AV_COMBINER))

typedef struct _GstAVCombiner GstAVCombiner;
typedef struct _GstAVCombinerClass GstAVCombinerClass;

struct _GstAVCombiner
{
  GstAggregator parent;

  GstPad *audio_sinkpad, *video_sinkpad;
  GstCaps *audio_caps, *video_caps;
};

struct _GstAVCombinerClass
{
  GstAggregatorClass parent_class;
};

G_GNUC_INTERNAL
GType gst_av_combiner_get_type (void);

G_END_DECLS

#endif /* _GST_AV_COMBINER_H_ */
