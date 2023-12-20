project ('gst-roq', 'c', version : '0.0.1', license : 'LGPL')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_version = meson.project_version()

api_version = '1.0'

gst_dep = dependency('gstreamer-1.0',
  version : '>=1.20',
  required : true,
  fallback : ['gstreamer', 'gst_dep'])

quiclib_dep = dependency('gstquiclib',
  required : true,
  fallback : ['gst-quic-transport', 'gst_quic_transport_dep'])

quicutil_dep = dependency('gstquicutil',
  required : true,
  fallback : ['gst-quic-transport', 'gst_quic_transport_dep'])

quicstream_dep = dependency('gstquicstream',
  required : true,
  fallback : ['gst-quic-transport', 'gst_quic_transport_dep'])

quicdatagram_dep = dependency('gstquicdatagram',
  required : true,
  fallback : ['gst-quic-transport', 'gst_quic_transport_dep'])

subdir('elements')
