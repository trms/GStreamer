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
#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#ifndef INITGUID
#include <initguid.h>
#endif /* INITGUID */
#include <DeckLinkAPI_i.c>
#else
#include <DeckLinkAPI_v10_11.h>
#include <DeckLinkAPI_v11_5.h>
#include <DeckLinkAPI_v11_5_1.h>
#include <DeckLinkAPIConfiguration_v10_11.h>
#include <DeckLinkAPIVideoInput_v10_11.h>
#include <DeckLinkAPIVideoInput_v11_4.h>
#include <DeckLinkAPIVideoInput_v11_5_1.h>
#include <DeckLinkAPIVideoOutput_v10_11.h>
#include <DeckLinkAPIVideoOutput_v11_4.h>
#endif /* G_OS_WIN32 */

#include <DeckLinkAPI.h>

#if defined(G_OS_WIN32)
#define dlbool_t    BOOL
#define dlstring_t  BSTR
#elif defined(__APPLE__)
#define dlbool_t    bool
#define dlstring_t  CFStringRef
#else
#define dlbool_t    bool
#define dlstring_t  const char*
#endif

G_BEGIN_DECLS

typedef struct _GstDeckLink2AudioMeta GstDeckLink2AudioMeta;
typedef struct _GstDeckLink2DisplayMode GstDeckLink2DisplayMode;

typedef enum
{
  GST_DECKLINK2_API_LEVEL_UNKNOWN,
  GST_DECKLINK2_API_LEVEL_10_11,
  GST_DECKLINK2_API_LEVEL_11_4,
  GST_DECKLINK2_API_LEVEL_11_5_1,
  GST_DECKLINK2_API_LEVEL_LATEST,
} GstDeckLink2APILevel;

/* defines custom display mode for wide screen */
#define bmdModeNTSC_W     ((BMDDisplayMode) 0x4E545343) /* 'NTSC' */
#define bmdModeNTSC2398_W ((BMDDisplayMode) 0x4E543233) /* 'NT23' */
#define bmdModePAL_W      ((BMDDisplayMode) 0x50414C20) /* 'PAL ' */
#define bmdModeNTSCp_W    ((BMDDisplayMode) 0x4E545350) /* 'NTSP' */
#define bmdModePALp_W     ((BMDDisplayMode) 0x50414C50) /* 'PALP' */

#define GST_TYPE_DECKLINK2_MODE (gst_decklink2_mode_get_type ())
GType gst_decklink2_mode_get_type (void);

#define GST_TYPE_DECKLINK2_VIDEO_FORMAT (gst_decklink2_video_format_get_type ())
GType gst_decklink2_video_format_get_type (void);

#define bmdProfileDefault ((BMDProfileID) 0)
#define GST_TYPE_DECKLINK2_PROFILE_ID (gst_decklink2_profile_id_get_type ())
GType gst_decklink2_profile_id_get_type (void);

typedef enum
{
  GST_DECKLINK2_KEYER_MODE_OFF,
  GST_DECKLINK2_KEYER_MODE_INTERNAL,
  GST_DECKLINK2_KEYER_MODE_EXTERNAL
} GstDeckLink2KeyerMode;
#define GST_TYPE_DECKLINK2_KEYER_MODE (gst_decklink2_keyer_mode_get_type ())
GType gst_decklink2_keyer_mode_get_type (void);

typedef enum
{
  GST_DECKLINK2_MAPPING_FORMAT_DEFAULT,
  GST_DECKLINK2_MAPPING_FORMAT_LEVEL_A, /* bmdDeckLinkConfigSMPTELevelAOutput = true */
  GST_DECKLINK2_MAPPING_FORMAT_LEVEL_B, /* bmdDeckLinkConfigSMPTELevelAOutput = false */
} GstDeckLink2MappingFormat;
#define GST_TYPE_DECKLINK2_MAPPING_FORMAT (gst_decklink2_mapping_format_get_type ())
GType gst_decklink2_mapping_format_get_type (void);

#define GST_TYPE_DECKLINK2_TIMECODE_FORMAT (gst_decklink2_timecode_format_get_type ())
GType gst_decklink2_timecode_format_get_type (void);

#define GST_TYPE_DECKLINK2_VIDEO_CONNECTION (gst_decklink2_video_connection_get_type ())
GType gst_decklink2_video_connection_get_type (void);

#define bmdAudioConnectionUnspecified ((BMDAudioConnection) 0)
#define GST_TYPE_DECKLINK2_AUDIO_CONNECTION (gst_decklink2_audio_connection_get_type ())
GType gst_decklink2_audio_connection_get_type (void);

typedef enum
{
  GST_DECKLINK2_AUDIO_CHANNELS_DISABLED = -1,
  GST_DECKLINK2_AUDIO_CHANNELS_MAX = 0,
  GST_DECKLINK2_AUDIO_CHANNELS_2 = 2,
  GST_DECKLINK2_AUDIO_CHANNELS_8 = 8,
  GST_DECKLINK2_AUDIO_CHANNELS_16 = 16,
} GstDeckLink2AudioChannels;
#define GST_TYPE_DECKLINK2_AUDIO_CHANNELS (gst_decklink2_audio_channels_get_type ())
GType gst_decklink2_audio_channels_get_type (void);

struct _GstDeckLink2DisplayMode
{
  BMDDisplayMode mode;
  gint width;
  gint height;
  gint fps_n;
  gint fps_d;
  gboolean interlaced;
  gint par_n;
  gint par_d;
  gboolean tff;
};

gboolean  gst_decklink2_init_once (void);

void      gst_decklink2_deinit (void);

gboolean  gst_decklink2_get_api_version (guint * major,
                                         guint * minor,
                                         guint * sub,
                                         guint * extra);

GstDeckLink2APILevel gst_decklink2_get_api_level (void);

const gchar * gst_decklink2_api_level_to_string (GstDeckLink2APILevel level);

GstCaps * gst_decklink2_get_default_template_caps (void);

BMDDisplayMode gst_decklink2_get_real_display_mode (BMDDisplayMode mode);

typedef gboolean (*GstDeckLink2DoesSupportVideoMode) (GstObject * object,
                                                      BMDDisplayMode mode,
                                                      BMDPixelFormat format);

GstCaps * gst_decklink2_build_caps (GstObject * io_object,
                                    IDeckLinkDisplayModeIterator * iter,
                                    BMDDisplayMode requested_mode,
                                    BMDPixelFormat format,
                                    GstDeckLink2DoesSupportVideoMode func);

GstCaps * gst_decklink2_build_template_caps (GstObject * io_object,
                                             IDeckLinkDisplayModeIterator * iter,
                                             GstDeckLink2DoesSupportVideoMode func,
                                             GArray * format_table);

GstCaps * gst_decklink2_get_caps_from_mode (const GstDeckLink2DisplayMode * mode);


GstVideoFormat gst_decklink2_video_format_from_pixel_format (BMDPixelFormat format);

BMDPixelFormat gst_decklink2_pixel_format_from_video_format (GstVideoFormat format);


struct _GstDeckLink2AudioMeta
{
  GstMeta meta;

  GstSample *sample;
};

GType gst_decklink2_audio_meta_api_get_type (void);
#define GST_DECKLINK2_AUDIO_META_API_TYPE (gst_decklink2_audio_meta_api_get_type())

const GstMetaInfo *gst_decklink2_audio_meta_get_info (void);
#define GST_DECKLINK2_AUDIO_META_INFO (gst_decklink2_audio_meta_get_info())

#define gst_buffer_get_decklink2_audio_meta(b) \
    ((GstDeckLink2AudioMeta*)gst_buffer_get_meta((b),GST_DECKLINK2_AUDIO_META_API_TYPE))

GstDeckLink2AudioMeta * gst_buffer_add_decklink2_audio_meta (GstBuffer * buffer,
                                                             GstSample * audio_sample);

static inline gboolean
_gst_decklink2_result (HRESULT hr, GstDebugCategory * cat, const gchar * file,
    const gchar * function, gint line)
{
  if (hr == S_OK)
    return TRUE;

#ifndef GST_DISABLE_GST_DEBUG
#ifdef G_OS_WIN32
  {
    gchar *error_text = g_win32_error_message ((guint) hr);
    gst_debug_log (cat, GST_LEVEL_WARNING, file, function, line, NULL,
        "DeckLink call failed: 0x%x (%s)", (guint) hr,
        GST_STR_NULL (error_text));
    g_free (error_text);
  }
#else
  gst_debug_log (cat, GST_LEVEL_WARNING, file, function, line, NULL,
      "DeckLink call failed: 0x%x", (guint) hr);
#endif /* G_OS_WIN32 */
#endif /* GST_DISABLE_GST_DEBUG */

  return FALSE;
}

#define gst_decklink2_result(hr) \
  _gst_decklink2_result (hr, GST_CAT_DEFAULT, __FILE__, GST_FUNCTION, __LINE__)

#define GST_DECKLINK2_PRINT_CHAR(c) \
  g_ascii_isprint(c) ? (c) : '.'

#define GST_DECKLINK2_FOURCC_ARGS(fourcc) \
  GST_DECKLINK2_PRINT_CHAR(((fourcc) >> 24) & 0xff), \
  GST_DECKLINK2_PRINT_CHAR(((fourcc) >> 16) & 0xff), \
  GST_DECKLINK2_PRINT_CHAR(((fourcc) >> 8) & 0xff), \
  GST_DECKLINK2_PRINT_CHAR((fourcc) & 0xff)

G_END_DECLS

#ifdef __cplusplus
#include <string>
#include <functional>
#include <stdint.h>

#define GST_DECKLINK2_CLEAR_COM(obj) G_STMT_START { \
    if (obj) { \
      (obj)->Release (); \
      (obj) = nullptr; \
    } \
  } G_STMT_END

#ifndef G_OS_WIN32
#include <string.h>
inline bool operator==(REFIID a, REFIID b)
{
  if (memcmp (&a, &b, sizeof (REFIID)) != 0)
    return false;

  return true;
}
#endif

#if defined(G_OS_WIN32)
const std::function<void(dlstring_t)> DeleteString = SysFreeString;

const std::function<std::string(dlstring_t)> DlToStdString = [](dlstring_t dl_str) -> std::string
{
  int wlen = ::SysStringLen(dl_str);
  int mblen = ::WideCharToMultiByte(CP_ACP, 0, (wchar_t*)dl_str, wlen, NULL, 0, NULL, NULL);

  std::string ret_str(mblen, '\0');
  mblen = ::WideCharToMultiByte(CP_ACP, 0, (wchar_t*)dl_str, wlen, &ret_str[0], mblen, NULL, NULL);

  return ret_str;
};

const std::function<dlstring_t(std::string)> StdToDlString = [](std::string std_str) -> dlstring_t
{
  int wlen = ::MultiByteToWideChar(CP_ACP, 0, std_str.data(), (int)std_str.length(), NULL, 0);

  dlstring_t ret_str = ::SysAllocStringLen(NULL, wlen);
  ::MultiByteToWideChar(CP_ACP, 0, std_str.data(), (int)std_str.length(), ret_str, wlen);

  return ret_str;
};
#elif defined(__APPLE__)
const auto DeleteString = CFRelease;

const auto DlToStdString = [](dlstring_t dl_str) -> std::string
{
  std::string returnString("");
  CFIndex stringSize = CFStringGetLength(dl_str) + 1;
  char stringBuffer[stringSize];
  if (CFStringGetCString(dl_str, stringBuffer, stringSize, kCFStringEncodingUTF8))
    returnString = stringBuffer;
  return returnString;
};

const auto StdToDlString = [](std::string std_str) -> dlstring_t
{
  return CFStringCreateWithCString(kCFAllocatorMalloc, std_str.c_str(), kCFStringEncodingUTF8);
};
#else
#include <string.h>

const std::function<void(dlstring_t)> DeleteString = [](dlstring_t dl_str)
{
  free((void*)dl_str);
};

const std::function<std::string(dlstring_t)> DlToStdString = [](dlstring_t dl_str) -> std::string
{
  return dl_str;
};

const std::function<dlstring_t(std::string)> StdToDlString = [](std::string std_str) -> dlstring_t
{
  return strcpy((char*)malloc(std_str.length()+1), std_str.c_str());
};
#endif
#endif /* __cplusplus */
