/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstm3u8playlist.c:
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

#include "gstm3u8playlist.h"
#include "gsthlselements.h"

#define GST_CAT_DEFAULT hls_debug

GType
gst_m3u8_playlist_type_get_type (void)
{
  static GType type = 0;
  static const GEnumValue playlist_types[] = {
    {GST_M3U8_PLAYLIST_TYPE_UNSPECIFIED, "Unspecified", "unspecified"},
    {GST_M3U8_PLAYLIST_TYPE_EVENT, "Event", "event"},
    {GST_M3U8_PLAYLIST_TYPE_VOD, "Vod", "vod"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType _type = g_enum_register_static ("GstM3U8PlaylistType",
        playlist_types);
    g_once_init_leave (&type, _type);
  }

  return type;
}

typedef struct _GstM3U8Entry GstM3U8Entry;

struct _GstM3U8Entry
{
  gfloat duration;
  gchar *title;
  gchar *url;
  gboolean discontinuous;
};

static GstM3U8Entry *
gst_m3u8_entry_new (const gchar * url, const gchar * title,
    gfloat duration, gboolean discontinuous)
{
  GstM3U8Entry *entry;

  g_return_val_if_fail (url != NULL, NULL);

  entry = g_new0 (GstM3U8Entry, 1);
  entry->url = g_strdup (url);
  entry->title = g_strdup (title);
  entry->duration = duration;
  entry->discontinuous = discontinuous;
  return entry;
}

static void
gst_m3u8_entry_free (GstM3U8Entry * entry)
{
  g_return_if_fail (entry != NULL);

  g_free (entry->url);
  g_free (entry->title);
  g_free (entry);
}

GstM3U8Playlist *
gst_m3u8_playlist_new (guint version, guint window_size)
{
  return gst_m3u8_playlist_new_full (version, window_size, NULL);
}

GstM3U8Playlist *
gst_m3u8_playlist_new_full (guint version, guint window_size,
    const gchar * duration_format)
{
  GstM3U8Playlist *playlist;

  playlist = g_new0 (GstM3U8Playlist, 1);
  playlist->version = version;
  playlist->window_size = window_size;
  playlist->type = GST_M3U8_PLAYLIST_TYPE_UNSPECIFIED;
  playlist->end_list = FALSE;
  playlist->entries = g_queue_new ();

  if (duration_format) {
    playlist->duration_format = g_strdup (duration_format);
  } else {
    /* The same as g_ascii_dtostr() */
    playlist->duration_format = g_strdup ("%.17g");
  }

  return playlist;
}

void
gst_m3u8_playlist_free (GstM3U8Playlist * playlist)
{
  g_return_if_fail (playlist != NULL);

  g_queue_foreach (playlist->entries, (GFunc) gst_m3u8_entry_free, NULL);
  g_queue_free (playlist->entries);
  g_free (playlist->duration_format);
  g_free (playlist);
}


gboolean
gst_m3u8_playlist_add_entry (GstM3U8Playlist * playlist,
    const gchar * url, const gchar * title,
    gfloat duration, guint index, gboolean discontinuous)
{
  GstM3U8Entry *entry;

  g_return_val_if_fail (playlist != NULL, FALSE);
  g_return_val_if_fail (url != NULL, FALSE);

  entry = gst_m3u8_entry_new (url, title, duration, discontinuous);

  if (playlist->window_size > 0) {
    /* Delete old entries from the playlist */
    while (playlist->entries->length >= playlist->window_size) {
      GstM3U8Entry *old_entry;

      old_entry = g_queue_pop_head (playlist->entries);
      gst_m3u8_entry_free (old_entry);
    }
  }

  playlist->sequence_number = index + 1;
  g_queue_push_tail (playlist->entries, entry);

  return TRUE;
}

static guint
gst_m3u8_playlist_target_duration (GstM3U8Playlist * playlist)
{
  guint64 target_duration = 0;
  GList *l;

  for (l = playlist->entries->head; l != NULL; l = l->next) {
    GstM3U8Entry *entry = l->data;

    if (entry->duration > target_duration)
      target_duration = entry->duration;
  }

  return (guint) ((target_duration + 500 * GST_MSECOND) / GST_SECOND);
}

gchar *
gst_m3u8_playlist_render (GstM3U8Playlist * playlist)
{
  GString *playlist_str;
  GList *l;

  g_return_val_if_fail (playlist != NULL, NULL);

  playlist_str = g_string_new ("#EXTM3U\n");

  g_string_append_printf (playlist_str, "#EXT-X-VERSION:%d\n",
      playlist->version);

  g_string_append_printf (playlist_str, "#EXT-X-MEDIA-SEQUENCE:%d\n",
      playlist->sequence_number - playlist->entries->length);

  g_string_append_printf (playlist_str, "#EXT-X-TARGETDURATION:%u\n",
      gst_m3u8_playlist_target_duration (playlist));

  switch (playlist->type) {
    case GST_M3U8_PLAYLIST_TYPE_EVENT:
      g_string_append_printf (playlist_str, "#EXT-X-PLAYLIST-TYPE:%s\n",
          "EVENT");
      break;
    case GST_M3U8_PLAYLIST_TYPE_VOD:
      g_string_append_printf (playlist_str, "#EXT-X-PLAYLIST-TYPE::%s\n",
          "VOD");
      break;
    default:
      break;
  }

  g_string_append (playlist_str, "\n");

  /* Entries */
  for (l = playlist->entries->head; l != NULL; l = l->next) {
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
    GstM3U8Entry *entry = l->data;

    if (entry->discontinuous)
      g_string_append (playlist_str, "#EXT-X-DISCONTINUITY\n");

    if (playlist->version < 3) {
      g_string_append_printf (playlist_str, "#EXTINF:%d,%s\n",
          (gint) ((entry->duration + 500 * GST_MSECOND) / GST_SECOND),
          entry->title ? entry->title : "");
    } else {
      g_string_append_printf (playlist_str, "#EXTINF:%s,%s\n",
          g_ascii_formatd (buf, sizeof (buf), playlist->duration_format,
              entry->duration / GST_SECOND), entry->title ? entry->title : "");
    }

    g_string_append_printf (playlist_str, "%s\n", entry->url);
  }

  if (playlist->end_list)
    g_string_append (playlist_str, "#EXT-X-ENDLIST");

  return g_string_free (playlist_str, FALSE);
}
