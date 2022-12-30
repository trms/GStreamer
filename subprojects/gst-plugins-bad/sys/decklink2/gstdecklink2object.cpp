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

#include "gstdecklink2object.h"
#include "gstdecklink2deviceprovider.h"
#include <stdlib.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string>

GST_DEBUG_CATEGORY_EXTERN (gst_decklink2_debug);
#define GST_CAT_DEFAULT gst_decklink2_debug

/* *INDENT-OFF* */
static std::vector<GstDeckLink2Object *> device_list;
static std::mutex device_lock;
/* *INDENT-ON* */

struct _GstDeckLink2Object
{
  GstObject parent;

  GstDeckLink2APILevel api_level;

  IDeckLink *device;
  IDeckLinkProfileAttributes *attr;
  IDeckLinkAttributes_v10_11 *attr_10_11;
  IDeckLinkConfiguration *config;
  IDeckLinkConfiguration_v10_11 *config_10_11;
  IDeckLinkProfileManager *profile_manager;

  GstDeckLink2Input *input;
  GstDevice *input_device;

  GstDeckLink2Output *output;
  GstDevice *output_device;

  guint device_number;
  gint64 persistent_id;
  gchar *serial_number;
  gchar *model_name;
  gchar *display_name;

  gboolean input_acquired;
  gboolean output_acquired;
};

static void gst_decklink2_object_dispose (GObject * object);
static void gst_decklink2_object_finalize (GObject * object);

#define gst_decklink2_object_parent_class parent_class
G_DEFINE_TYPE (GstDeckLink2Object, gst_decklink2_object, GST_TYPE_OBJECT);

static void
gst_decklink2_object_class_init (GstDeckLink2ObjectClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_decklink2_object_dispose;
  object_class->finalize = gst_decklink2_object_finalize;
}

static void
gst_decklink2_object_init (GstDeckLink2Object * self)
{
}

static void
gst_decklink2_object_dispose (GObject * object)
{
  GstDeckLink2Object *self = GST_DECKLINK2_OBJECT (object);

  if (self->input) {
    gst_object_unparent (GST_OBJECT (self->input));
    self->input = NULL;
  }

  if (self->output) {
    gst_object_unparent (GST_OBJECT (self->output));
    self->output = NULL;
  }

  gst_clear_object (&self->input_device);
  gst_clear_object (&self->output_device);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_decklink2_object_finalize (GObject * object)
{
  GstDeckLink2Object *self = GST_DECKLINK2_OBJECT (object);

  GST_DECKLINK2_CLEAR_COM (self->attr);
  GST_DECKLINK2_CLEAR_COM (self->attr_10_11);
  GST_DECKLINK2_CLEAR_COM (self->config);
  GST_DECKLINK2_CLEAR_COM (self->config_10_11);
  GST_DECKLINK2_CLEAR_COM (self->profile_manager);
  GST_DECKLINK2_CLEAR_COM (self->device);

  g_free (self->serial_number);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstDeckLink2Object *
gst_decklink2_object_new (IDeckLink * device, guint index,
    GstDeckLink2APILevel api_level, const gchar * api_ver_str,
    const gchar * driver_ver_str)
{
  HRESULT hr;
  GstDeckLink2Input *input;
  GstDeckLink2Output *output;
  GstDeckLink2Object *self;
  dlstring_t str;
  gint64 max_num_audio_channels = 0;
  GstCaps *caps = NULL;

  input = gst_decklink2_input_new (device, api_level);
  output = gst_decklink2_output_new (device, api_level);

  if (!output && !input)
    return NULL;

  self = (GstDeckLink2Object *)
      g_object_new (GST_TYPE_DECKLINK2_OBJECT, NULL);
  gst_object_ref_sink (self);

  self->api_level = api_level;

  if (input)
    gst_object_set_parent (GST_OBJECT (input), GST_OBJECT (self));

  if (output)
    gst_object_set_parent (GST_OBJECT (output), GST_OBJECT (self));

  self->input = input;
  self->output = output;
  self->device_number = index;
  self->device = device;
  device->AddRef ();

  if (api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    IDeckLinkConfiguration_v10_11 *config_10_11 = NULL;
    hr = device->QueryInterface (IID_IDeckLinkConfiguration_v10_11,
        (void **) &config_10_11);
    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Couldn't get config object");
      gst_object_unref (self);
      return NULL;
    }

    hr = config_10_11->GetString
        (bmdDeckLinkConfigDeviceInformationSerialNumber, &str);

    if (gst_decklink2_result (hr)) {
      std::string serial_number = DlToStdString (str);
      DeleteString (str);

      self->serial_number = g_strdup (serial_number.c_str ());
      GST_DEBUG_OBJECT (self, "device %d has serial number %s", index,
          GST_STR_NULL (self->serial_number));
    }

    self->config_10_11 = config_10_11;
  } else {
    IDeckLinkConfiguration *config = NULL;
    hr = device->QueryInterface (IID_IDeckLinkConfiguration, (void **) &config);
    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Couldn't get config object");
      gst_object_unref (self);
      return NULL;
    }

    hr = config->GetString (bmdDeckLinkConfigDeviceInformationSerialNumber,
        &str);
    if (gst_decklink2_result (hr)) {
      std::string serial_number = DlToStdString (str);
      DeleteString (str);

      self->serial_number = g_strdup (serial_number.c_str ());
      GST_DEBUG_OBJECT (self, "device %d has serial number %s", index,
          GST_STR_NULL (self->serial_number));
    }

    self->config = config;
    device->QueryInterface (IID_IDeckLinkProfileManager,
        (void **) &self->profile_manager);
  }

  if (api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    hr = device->QueryInterface (IID_IDeckLinkAttributes_v10_11,
        (void **) &self->attr_10_11);
  } else {
    hr = device->QueryInterface (IID_IDeckLinkProfileAttributes,
        (void **) &self->attr);
  }

  if (!gst_decklink2_result (hr)) {
    GST_WARNING_OBJECT (self,
        "IDeckLinkProfileAttributes interface is not available");
    self->persistent_id = self->device_number;
  } else {
    hr = E_FAIL;
    if (self->attr) {
      hr = self->attr->GetInt (BMDDeckLinkPersistentID, &self->persistent_id);
      if (!gst_decklink2_result (hr))
        self->persistent_id = self->device_number;
      hr = self->attr->GetInt (BMDDeckLinkMaximumAudioChannels,
          &max_num_audio_channels);
    } else if (self->attr_10_11) {
      hr = self->attr_10_11->GetInt (BMDDeckLinkPersistentID,
          &self->persistent_id);
      if (!gst_decklink2_result (hr))
        self->persistent_id = self->device_number;
      hr = self->attr_10_11->GetInt (BMDDeckLinkMaximumAudioChannels,
          &max_num_audio_channels);
    }

    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (self, "Couldn't query max audio channels");
      max_num_audio_channels = 0;
    }
  }

  hr = device->GetModelName (&str);
  if (gst_decklink2_result (hr)) {
    std::string model_name = DlToStdString (str);
    DeleteString (str);

    self->model_name = g_strdup (model_name.c_str ());
  }

  hr = device->GetDisplayName (&str);
  if (gst_decklink2_result (hr)) {
    std::string display_name = DlToStdString (str);
    DeleteString (str);

    self->display_name = g_strdup (display_name.c_str ());
  }

  if (self->input) {
    caps = gst_decklink2_input_get_caps (self->input,
        bmdModeUnknown, bmdFormatUnspecified);
    self->input_device = gst_decklink2_device_new (TRUE, self->model_name,
        self->display_name, self->serial_number, caps, self->persistent_id,
        self->device_number, (guint) max_num_audio_channels, driver_ver_str,
        api_ver_str);
    gst_clear_caps (&caps);
    gst_object_ref_sink (self->input_device);
  }

  if (self->output) {
    caps = gst_decklink2_output_get_caps (self->output,
        bmdModeUnknown, bmdFormatUnspecified);
    self->output_device = gst_decklink2_device_new (FALSE, self->model_name,
        self->display_name, self->serial_number, caps, self->persistent_id,
        self->device_number, (guint) max_num_audio_channels, driver_ver_str,
        api_ver_str);
    gst_clear_caps (&caps);
    gst_object_ref_sink (self->output_device);
  }

  return self;
}

#ifdef G_OS_WIN32
static IDeckLinkIterator *
CreateDeckLinkIteratorInstance (void)
{
  IDeckLinkIterator *iter = NULL;
  CoCreateInstance (CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL,
      IID_IDeckLinkIterator, (void **) &iter);

  return iter;
}

static IDeckLinkIterator *
CreateDeckLinkIteratorInstance_v10_11 (void)
{
  IDeckLinkIterator *iter = NULL;
  CoCreateInstance (CLSID_CDeckLinkIterator_v10_11, NULL, CLSCTX_ALL,
      IID_IDeckLinkIterator, (void **) &iter);

  return iter;
}
#endif

static void
gst_decklink2_device_init (void)
{
  GstDeckLink2APILevel api_level = gst_decklink2_get_api_level ();
  IDeckLinkIterator *iter = NULL;
  HRESULT hr = S_OK;
  guint major, minor, sub, extra;
  std::string driver_version;
  std::string api_version;

  if (api_level == GST_DECKLINK2_API_LEVEL_UNKNOWN)
    return;

  api_version = gst_decklink2_api_level_to_string (api_level);

  gst_decklink2_get_api_version (&major, &minor, &sub, &extra);
  driver_version = std::to_string (major) + "." + std::to_string (minor)
      + "." + std::to_string (sub) + "." + std::to_string (extra);

  if (api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    iter = CreateDeckLinkIteratorInstance_v10_11 ();
  } else {
    iter = CreateDeckLinkIteratorInstance ();
  }

  if (!iter) {
    GST_DEBUG ("Couldn't create device iterator");
    return;
  }

  guint i = 0;
  do {
    IDeckLink *device = NULL;
    GstDeckLink2Object *object;
    hr = iter->Next (&device);
    if (!gst_decklink2_result (hr))
      break;

    object = gst_decklink2_object_new (device, i, api_level,
        api_version.c_str (), driver_version.c_str ());
    device->Release ();

    if (object)
      device_list.push_back (object);

    i++;
  } while (gst_decklink2_result (hr));

  iter->Release ();

  std::sort (device_list.begin (), device_list.end (),
      [](const GstDeckLink2Object * a, const GstDeckLink2Object * b)->bool
      {
        {
          return a->persistent_id < b->persistent_id;
        }
      }
  );

  GST_DEBUG ("Found %u device", (guint) device_list.size ());
}

static void
gst_decklink2_device_init_once (void)
{
  GST_DECKLINK2_CALL_ONCE_BEGIN {
    std::lock_guard < std::mutex > lk (device_lock);
    gst_decklink2_device_init ();
  } GST_DECKLINK2_CALL_ONCE_END;
}

GstDeckLink2Input *
gst_decklink2_acquire_input (guint device_number, gint64 persistent_id)
{
  GstDeckLink2Object *target = NULL;

  gst_decklink2_device_init_once ();

  std::lock_guard < std::mutex > lk (device_lock);

  /* *INDENT-OFF* */
  if (persistent_id != -1) {
    auto object = std::find_if (device_list.begin (), device_list.end (),
      [&](const GstDeckLink2Object * obj) {
        return obj->persistent_id == persistent_id;
      });

    if (object == device_list.end ()) {
      GST_WARNING ("Couldn't find object for persistent id %" G_GINT64_FORMAT,
          persistent_id);
      return NULL;
    }

    target = *object;
  }

  if (!target) {
    auto object = std::find_if (device_list.begin (), device_list.end (),
        [&](const GstDeckLink2Object * obj) {
          return obj->device_number == device_number;
        });

    if (object == device_list.end ()) {
      GST_WARNING ("Couldn't find object for device number %u", device_number);
      return NULL;
    }

    target = *object;
  }
  /* *INDENT-ON* */

  if (!target->input) {
    GST_WARNING_OBJECT (target, "Device does not support input");
    return NULL;
  }

  if (target->input_acquired) {
    GST_WARNING_OBJECT (target, "Input was already acquired");
    return NULL;
  }

  target->input_acquired = TRUE;
  return (GstDeckLink2Input *) gst_object_ref (target->input);
}

GstDeckLink2Output *
gst_decklink2_acquire_output (guint device_number, gint64 persistent_id)
{
  GstDeckLink2Object *target = NULL;

  gst_decklink2_device_init_once ();

  std::lock_guard < std::mutex > lk (device_lock);

  /* *INDENT-OFF* */
  if (persistent_id != -1) {
    auto object = std::find_if (device_list.begin (), device_list.end (),
      [&](const GstDeckLink2Object * obj) {
        return obj->persistent_id == persistent_id;
      });

    if (object == device_list.end ()) {
      GST_WARNING ("Couldn't find object for persistent id %" G_GINT64_FORMAT,
          persistent_id);
      return NULL;
    }

    target = *object;
  }

  if (!target) {
    auto object = std::find_if (device_list.begin (), device_list.end (),
        [&](const GstDeckLink2Object * obj) {
          return obj->device_number == device_number;
        });

    if (object == device_list.end ()) {
      GST_WARNING ("Couldn't find object for device number %u", device_number);
      return NULL;
    }

    target = *object;
  }
  /* *INDENT-ON* */

  if (!target->output) {
    GST_WARNING_OBJECT (target, "Device does not support output");
    return NULL;
  }

  if (target->output_acquired) {
    GST_WARNING_OBJECT (target, "Output was already acquired");
    return NULL;
  }

  target->output_acquired = TRUE;
  return (GstDeckLink2Output *) gst_object_ref (target->output);
}

void
gst_decklink2_release_input (GstDeckLink2Input * input)
{
  std::unique_lock < std::mutex > lk (device_lock);
  auto object = std::find_if (device_list.begin (), device_list.end (),
      [&](const GstDeckLink2Object * obj) {
        return obj->input == input;
      }
  );

  if (object == device_list.end ()) {
    GST_ERROR_OBJECT (input, "Couldn't find parent object");
  } else {
    (*object)->input_acquired = FALSE;
  }
  lk.unlock ();

  gst_object_unref (input);
}

void
gst_decklink2_release_output (GstDeckLink2Output * output)
{
  std::unique_lock < std::mutex > lk (device_lock);
  auto object = std::find_if (device_list.begin (), device_list.end (),
      [&](const GstDeckLink2Object * obj) {
        return obj->output == output;
      }
  );

  if (object == device_list.end ()) {
    GST_ERROR_OBJECT (output, "Couldn't find parent object");
  } else {
    (*object)->output_acquired = FALSE;
  }
  lk.unlock ();

  gst_object_unref (output);
}

void
gst_decklink2_object_deinit (void)
{
  std::lock_guard < std::mutex > lk (device_lock);

  /* *INDENT-OFF* */
  for (auto iter : device_list)
    gst_object_unref (iter);
  /* *INDENT-ON* */

  device_list.clear ();
}

GList *
gst_decklink2_get_devices (void)
{
  GList *list = NULL;

  gst_decklink2_device_init_once ();

  std::lock_guard < std::mutex > lk (device_lock);

  /* *INDENT-OFF* */
  for (auto iter : device_list) {
    if (iter->input_device)
      list = g_list_append (list, gst_object_ref (iter->input_device));

    if (iter->output_device)
      list = g_list_append (list, gst_object_ref (iter->output_device));
  }
  /* *INDENT-ON* */

  return list;
}

static HRESULT
gst_decklink2_set_duplex_mode (gint64 persistent_id, BMDDuplexMode_v10_11 mode)
{
  GstDeckLink2Object *object = NULL;
  HRESULT hr = E_FAIL;
  dlbool_t duplex_supported = FALSE;

  std::lock_guard < std::mutex > lk (device_lock);

  /* *INDENT-OFF* */
  for (auto iter : device_list) {
    if (iter->persistent_id == persistent_id) {
      object = iter;
      break;
    }
  }
  /* *INDENT-ON* */

  if (!object) {
    GST_ERROR ("Couldn't find device for persistent id %" G_GINT64_FORMAT,
        persistent_id);
    return E_FAIL;
  }

  if (!object->attr_10_11 || !object->config_10_11) {
    GST_WARNING_OBJECT (object,
        "Couldn't set duplex mode, missing required interface");
    return E_FAIL;
  }

  hr = object->attr_10_11->GetFlag ((BMDDeckLinkAttributeID)
      BMDDeckLinkSupportsDuplexModeConfiguration_v10_11, &duplex_supported);
  if (!gst_decklink2_result (hr)) {
    GST_WARNING_OBJECT (object, "Couldn't query duplex mode support");
    return hr;
  }

  if (!duplex_supported) {
    GST_WARNING_OBJECT (object, "Duplex mode is not supported");
    return E_FAIL;
  }

  return object->config_10_11->SetInt ((BMDDeckLinkConfigurationID)
      bmdDeckLinkConfigDuplexMode_v10_11, mode);
}

HRESULT
gst_decklink2_object_set_profile_id (GstDeckLink2Object * object,
    BMDProfileID profile_id)
{
  gchar *profile_id_str = NULL;
  HRESULT hr = E_FAIL;

  g_return_val_if_fail (GST_IS_DECKLINK2_OBJECT (object), E_INVALIDARG);

  if (profile_id == bmdProfileDefault)
    return S_OK;

  profile_id_str = g_enum_to_string (GST_TYPE_DECKLINK2_PROFILE_ID, profile_id);

  GST_DEBUG_OBJECT (object, "Setting profile id \"%s\"", profile_id_str);

  if (object->api_level == GST_DECKLINK2_API_LEVEL_10_11) {
    dlbool_t duplex_supported = FALSE;
    BMDDuplexMode_v10_11 duplex_mode = bmdDuplexModeHalf_v10_11;

    if (!object->attr_10_11 || !object->config_10_11) {
      GST_WARNING_OBJECT (object,
          "Couldn't set duplex mode, missing required interface");
      hr = E_FAIL;
      goto out;
    }

    switch (profile_id) {
      case bmdProfileOneSubDeviceHalfDuplex:
      case bmdProfileTwoSubDevicesHalfDuplex:
      case bmdProfileFourSubDevicesHalfDuplex:
        duplex_mode = bmdDuplexModeHalf_v10_11;
        GST_DEBUG_OBJECT (object, "Mapping \"%s\" to bmdDuplexModeHalf");
        break;
      default:
        GST_DEBUG_OBJECT (object, "Mapping \"%s\" to bmdDuplexModeFull");
        duplex_mode = bmdDuplexModeFull_v10_11;
        break;
    }

    hr = object->attr_10_11->GetFlag ((BMDDeckLinkAttributeID)
        BMDDeckLinkSupportsDuplexModeConfiguration_v10_11, &duplex_supported);
    if (!gst_decklink2_result (hr))
      duplex_supported = FALSE;

    if (!duplex_supported) {
      gint64 paired_device_id = 0;
      if (duplex_mode == bmdDuplexModeFull_v10_11) {
        GST_WARNING_OBJECT (object, "Device does not support Full-Duplex-Mode");
        goto out;
      }

      hr = object->attr_10_11->GetInt ((BMDDeckLinkAttributeID)
          BMDDeckLinkPairedDevicePersistentID_v10_11, &paired_device_id);
      if (gst_decklink2_result (hr)) {
        GST_DEBUG_OBJECT (object,
            "Device has paired device, Setting duplex mode to paired device");
        hr = gst_decklink2_set_duplex_mode (paired_device_id, duplex_mode);
      } else {
        GST_WARNING_OBJECT (object, "Device does not support Half-Duplex-Mode");
      }
    } else {
      hr = object->config_10_11->SetInt ((BMDDeckLinkConfigurationID)
          bmdDeckLinkConfigDuplexMode_v10_11, duplex_mode);
    }

    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (object,
          "Couldn't set profile \"%s\"", profile_id_str);
    } else {
      GST_DEBUG_OBJECT (object, "Profile \"%s\" is configured", profile_id_str);
    }
  } else {
    IDeckLinkProfile *profile = NULL;

    if (!object->profile_manager) {
      GST_WARNING_OBJECT (object,
          "Profile \"%s\" is requested but profile manager is not available",
          profile_id_str);
      goto out;
    }

    hr = object->profile_manager->GetProfile (profile_id, &profile);
    if (gst_decklink2_result (hr)) {
      hr = profile->SetActive ();
      profile->Release ();
    }

    if (!gst_decklink2_result (hr)) {
      GST_WARNING_OBJECT (object,
          "Couldn't set profile \"%s\"", profile_id_str);
    } else {
      GST_DEBUG_OBJECT (object, "Profile \"%s\" is configured", profile_id_str);
    }
  }

out:
  g_free (profile_id_str);

  return hr;
}
