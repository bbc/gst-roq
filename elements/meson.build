plugin_c_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'gst-roq')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'RTP-over-QUIC Plug-ins for GStreamer')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://github.com/bbc/gst-roq')
configure_file(output : 'config.h', configuration : cdata)

rtpquicdemux_sources = [
  'gstrtpquicdemux.c'
  ]

gstrtpquicdemux = library('gstrtpquicdemux',
  rtpquicdemux_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep, quiclib_dep, quicutil_dep, quicstream_dep,
    quicdatagram_dep],
  install : true,
  install_dir : plugins_install_dir,
)

rtpquicmux_sources = [
  'gstrtpquicmux.c'
  ]

gstrtpquicmux = library('gstrtpquicmux',
  rtpquicmux_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep, quiclib_dep, quicutil_dep, quicstream_dep,
    quicdatagram_dep],
  install : true,
  install_dir : plugins_install_dir,
)

rtpquicmux_dep = declare_dependency(link_with: gstrtpquicmux)

roqsrcbin_sources = [
  'gstroqsrcbin.c'
  ]

gstroqsrcbin = library('gstroqsrcbin',
  roqsrcbin_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep, quiclib_dep, quicutil_dep],
  install : true,
  install_dir : plugins_install_dir,
)

roqsinkbin_sources = [
  'gstroqsinkbin.c'
  ]

gstroqsinkbin = library('gstroqsinkbin',
  roqsinkbin_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep, quiclib_dep, quicutil_dep, rtpquicmux_dep],
  install : true,
  install_dir : plugins_install_dir,
)
