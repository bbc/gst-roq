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

project ('gst-roq', 'c', version : '0.2.0', license : 'LGPL',
  default_options : ['c_std=c11', 'warning_level=everything'])

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
  fallback : ['gst-quic-transport', 'quiclib_dep'])

quicutil_dep = dependency('gstquicutil',
  required : true,
  fallback : ['gst-quic-transport', 'quicutils_dep'])

quicstream_dep = dependency('gstquicstream',
  required : true,
  fallback : ['gst-quic-transport', 'quicstream_dep'])

quicdatagram_dep = dependency('gstquicdatagram',
  required : true,
  fallback : ['gst-quic-transport', 'quicdatagram_dep'])

subdir('elements')
