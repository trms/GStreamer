# What to do when a plugin is missing

The mechanism and API described in this document requires GStreamer core
and gst-plugins-base versions \>= 0.10.12. Further information on some
aspects of this document can be found in the libgstbaseutils API
reference.

We only discuss playback pipelines for now.

A three step process:

# GStreamer level

Elements will use a "missing-plugin" element message to report
missing plugins, with the following fields set:

* **`type`**: (string) { "urisource", "urisink", "decoder", "encoder",
"element" } (we do not distinguish between demuxer/decoders/parsers etc.)

* **`detail`**: (string) or (caps) depending on the type { ANY } ex: "rtsp,
"rtspt", "audio/x-mp3,rate=48000,…"

* **`name`**: (string) { ANY } ex: "RTSP protocol handler",..

### missing uri handler

ex. rtsp://some.camera/stream1

When no protocol handler is installed for rtsp://, the application will not be
able to instantiate an element for that uri (`gst_element_make_from_uri()`
returns NULL).

Playbin will post a `missing-plugin` element message with the type set to
"urisource", detail set to "rtsp". Optionally the friendly name can be filled
in as well.

### missing typefind function

We don't recognize the type of the file, this should normally not happen
because all the typefinders are in the basic GStreamer installation.
There is not much useful information we can give about how to resolve this
issue. It is possible to use the first N bytes of the data to determine the
type (and needed plugin) on the server. We don't explore this option in this
document yet, but the proposal is flexible enough to accommodate this in the
future should the need arise.

### missing demuxer

Typically after running typefind on the data we determine the type of the
file. If there is no plugin found for the type, a `missing-plugin` element
message is posted by decodebin with the following fields: Type set to
"decoder", detail set to the caps for witch no plugin was found. Optionally
the friendly name can be filled in as well.

### missing decoder

The demuxer will dynamically create new pads with specific caps while it
figures out the contents of the container format. Decodebin tries to find the
decoders for these formats in the registry. If there is no decoder found, a
`missing-plugin` element message is posted by decodebin with the following
fields: Type set to "decoder", detail set to the caps for which no plugin
was found. Optionally the friendly name can be filled in as well. There is
no distinction made between the missing demuxer and decoder at the
application level.

### missing element

Decodebin and playbin will create a set of helper elements when they set up
their decoding pipeline. These elements are typically colorspace, sample rate,
audio sinks,... Their presence on the system is required for the functionality
of decodebin. It is typically a package dependency error if they are not
present but in case of a corrupted system the following `missing-plugin`
element message will be emitted: type set to "element", detail set to the
element factory name and the friendly name optionally set to a description
of the element's functionality in the decoding pipeline.

Except for reporting the missing plugins, no further policy is enforced at the
GStreamer level. It is up to the application to decide whether a missing
plugin constitutes a problem or not.

## Application level

The application's job is to listen for the `missing-plugin` element messages
and to decide on a policy to handle them. Following cases exist:

### partially missing plugins

The application will be able to complete a state change to PAUSED but there
will be a `missing-plugin` element message on the `GstBus`.

This means that it will be possible to play back part of the media file but not
all of it.

For example: suppose we have an .avi file with mp3 audio and divx video. If we
have the mp3 audio decoder but not the divx video decoder, it will be possible
to play only the audio part but not the video part. For an audio playback
application, this is not a problem but a video player might want to decide on:

  - require the use to install the additionally required plugins.
  - inform the user that only the audio will be played back
  - ask the user if it should download the additional codec or only play
    the audio part.
  - …

### completely unplayable stream

The application will receive an ERROR message from GStreamer informing it that
playback stopped (before it could reach PAUSED). This happens because none of
the streams is connected to a decoder. The error code and domain should be one
of the following in this case:

   - `GST_CORE_ERROR_MISSING_PLUGIN` (domain: `GST_CORE_ERROR`)
   - `GST_STREAM_ERROR_CODEC_NOT_FOUND` (domain: `GST_STREAM_ERROR`)

The application can then see that there are a set of `missing-plugin` element
messages on the `GstBus` and can decide to trigger the download procedure. It
does that as described in the following section.

`missing-plugin` element messages can be identified using the function
`gst_is_missing_plugin_message()`.

## Plugin download stage

At this point the application has
  - collected one or more `missing-plugin` element messages
  - made a decision that additional plugins should be installed

It will call a GStreamer utility function to convert each `missing-plugin`
message into an identifier string describing the missing capability. This is
done using the function `gst_missing_plugin_message_get_installer_detail()`.

The application will then pass these strings to `gst_install_plugins_async()`
or `gst_install_plugins_sync()` to initiate the download. See the API
documentation there (`libgstbaseutils`, part of `gst-plugins-base`) for more
details.

When new plugins have been installed, the application will have to initiate
a re-scan of the GStreamer plugin registry using `gst_update_registry()`.

### Format of the (UTF-8) string ID passed to the external installer system

The string is made up of several fields, separated by '|' characters.
The fields are:

- plugin system identifier, ie. "gstreamer" This identifier determines
the format of the rest of the detail string. Automatic plugin
installers should not process detail strings with unknown
identifiers. This allows other plugin-based libraries to use the
same mechanism for their automatic plugin installation needs, or for
the format to be changed should it turn out to be insufficient.

- plugin system version, e.g. "1.0" This is required so that when
there is a GStreamer-2.0 or GStreamer-3.0 at some point in future,
the different major versions can still co-exist and use the same
plugin install mechanism in the same way.

- application identifier, e.g. "totem" This may also be in the form of
"pid/12345" if the program name can’t be obtained for some reason.

- human-readable localised description of the required component, e.g.
"Vorbis audio decoder"

- identifier string for the required component, e.g.

- urisource-(`PROTOCOL_REQUIRED`) e.g. `urisource-http` or `urisource-rtsp`

- element-(`ELEMENT_REQUIRED`), e.g. `element-videoconvert`

- decoder-(`CAPS_REQUIRED`) e.g. `decoder-audio/x-vorbis` or
`decoder-application/ogg` or `decoder-audio/mpeg, mpegversion=(int)4` or
`decoder-video/mpeg, systemstream=(boolean)true, mpegversion=(int)2`

- encoder-(`CAPS_REQUIRED`) e.g. `encoder-audio/x-vorbis`

- optional further fields not yet specified

* An entire ID string might then look like this, for example:
`gstreamer|0.10|totem|Vorbis audio decoder|decoder-audio/x-vorbis`

* Plugin installers parsing this ID string should expect further fields also
separated by '|' symbols and either ignore them, warn the user, or error
out when encountering them.

* The human-readable description string is provided by the libgstbaseutils
library that can be found in gst-plugins-base versions >= 0.10.12 and can
also be used by demuxers to find out the codec names for taglists from given
caps in a unified and consistent way.

* Applications can create these detail strings using the function
`gst_missing_plugin_message_get_installer_detail()` on a given missing-plugin
message.

### Using missing-plugin messages for error reporting:

Missing-plugin messages are also useful for error reporting purposes, either in
the case where the application does not support libgimme-codec, or the external
installer is not available or not able to install the required plugins.

When creating error messages, applications may use the function
`gst_missing_plugin_message_get_description()` to obtain a possibly translated
description from each missing-plugin message (e.g. "Matroska demuxer" or
"Theora video depayloader"). This can be used to report to the user exactly
what it is that is missing.

## Notes for packagers

An easy way to introspect plugin .so files is:

```
$ gst-inspect --print-plugin-auto-install-info /path/to/libgstfoo.so
```

The output will be something like:

```
decoder-audio/x-vorbis
element-vorbisdec
element-vorbisenc
element-vorbisparse
element-vorbistag
encoder-audio/x-vorbis
```

BUT could also be like this (from the faad element in this case):

```
decoder-audio/mpeg, mpegversion=(int){ 2, 4 }
```

NOTE that this does not exactly match the caps string that the installer
will get from the application. The application will always ever ask for
one of

```
decoder-audio/mpeg, mpegversion=(int)2
decoder-audio/mpeg, mpegversion=(int)4
```

When introspecting, keep in mind that there are GStreamer plugins
that in turn load external plugins. Examples of these are pitfdll,
ladspa, or the GStreamer libvisual plugin. Those plugins will only
announce elements for the currently installed external plugins at
the time of introspection\! With the exception of pitfdll, this is
not really relevant to the playback case, but may become an issue in
future when applications like buzztard, jokosher or pitivi start
requestion elements by name, for example ladspa effect elements or
so.

This case could be handled if those wrapper plugins would also provide a
`gst-install-xxx-plugins-helper`, where xxx={ladspa|visual|...}. Thus if the
distro specific `gst-install-plugins-helper` can't resolve a request for e.g.
`element-bml-sonicverb` it can forward the request to
`gst-install-bml-plugins-helper` (bml is the buzz machine loader).

## Further references:

<http://gstreamer.freedesktop.org/data/doc/gstreamer/head/gst-plugins-base-libs/html/gstreamer-base-utils.html>
