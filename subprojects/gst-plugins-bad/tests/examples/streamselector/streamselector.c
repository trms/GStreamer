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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

static GMainLoop *loop = NULL;

typedef struct
{
  GstElement *selector;
  guint active_pad_num;
} SwitchData;

static gboolean
bus_msg (GstBus * bus, GstMessage * message, gpointer data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err;
      gchar *debug;

      gst_message_parse_error (message, &err, &debug);
      gst_println ("Error: %s", err->message);
      g_error_free (err);
      g_free (debug);

      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }

  return TRUE;
}

static gboolean
timer_cb (SwitchData * data)
{
  GstPad *to_active;

  if (data->active_pad_num == 0) {
    to_active = gst_element_get_static_pad (data->selector, "sink_1");
    data->active_pad_num = 1;
  } else {
    to_active = gst_element_get_static_pad (data->selector, "sink_0");
    data->active_pad_num = 0;
  }

  gst_println ("Switching to pad %d", data->active_pad_num);

  g_object_set (to_active, "active", TRUE, NULL);

  return G_SOURCE_CONTINUE;
}

int
main (int argc, char *argv[])
{
  GstElement *pipeline;
  GstBus *bus;
  SwitchData data;

  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  pipeline = gst_parse_launch ("streamselector name=selector ! videoconvert ! "
      "videoscale ! autovideosink videotestsrc ! "
      "video/x-raw,format=RGBA,width=640,height=480,framerate=30/1 ! queue ! "
      "selector. videotestsrc pattern=ball ! "
      "video/x-raw,format=NV12,width=320,height=240 ! queue ! "
      "selector.", NULL);

  if (!pipeline) {
    gst_printerrln ("Couldn't create pipeline");
    return 1;
  }

  data.selector = gst_bin_get_by_name (GST_BIN (pipeline), "selector");
  data.active_pad_num = 0;

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, bus_msg, NULL);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  g_timeout_add_seconds (3, (GSourceFunc) timer_cb, &data);

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (data.selector);
  gst_object_unref (pipeline);

  g_main_loop_unref (loop);

  gst_deinit ();

  return 0;
}
