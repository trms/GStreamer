sources = [
  'gstavcombiner.c',
  'plugin.c',
]

gstavcombiner = library('gstavcombiner',
  sources,
  c_args : gst_plugins_bad_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstvideo_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstavcombiner, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstavcombiner]
