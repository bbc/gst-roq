#
# Copyright (c) 2023 British Broadcasting Corporation - Research and Development
#
# Author: Sam Hurst <sam.hurst@bbc.co.uk>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#
# Alternatively, the contents of this file may be used under the
# GNU Lesser General Public License Version 2.1 (the "LGPL"), in
# which case the following provisions apply instead of the ones
# mentioned above:
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.
#

plugin_c_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'gst-roq')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'RTP-over-QUIC Plug-ins for GStreamer')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://github.com/bbc/gst-roq')
configure_file(output : 'config.h', configuration : cdata)

roqflowidmanager_sources = [
  'gstroqflowidmanager.c'
]

roqflowidmanager = library('roqflowidmanager',
  roqflowidmanager_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep],
  install: true,
  install_dir : plugins_install_dir
)

roqflowidmanager_dep = declare_dependency(link_with: roqflowidmanager)

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
    quicdatagram_dep, roqflowidmanager_dep],
  install : true,
  install_dir : plugins_install_dir,
)

rtpquicmux_dep = declare_dependency(link_with: gstrtpquicmux)

install_headers ('gstrtpquicmux.h', subdir : meson.project_name())

pkg_mod = import ('pkgconfig')

pkg_mod.generate(
  name : gstrtpquicmux.name(),
  filebase : gstrtpquicmux.name(),
  description : 'RTP-over-QUIC Muxer element',
  subdirs : gstrtpquicmux.name(),
  libraries : gstrtpquicmux
)

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
