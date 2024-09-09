/*
 * Copyright 2023 British Broadcasting Corporation - Research and Development
 *
 * Author: Sam Hurst <sam.hurst@bbc.co.uk>
 *
 * Based on the GStreamer template repository:
 *  https://gitlab.freedesktop.org/gstreamer/gst-template
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2020 Niels De Graef <niels.degraef@gmail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_RTPQUICMUX_H__
#define __GST_RTPQUICMUX_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_RTP_QUIC_MUX_TYPE_STREAM_BOUNDARY \
  gst_rtp_quic_mux_stream_boundary_get_type ()
GType gst_rtp_quic_mux_stream_boundary_get_type (void);
typedef enum _GstRtpQuicMuxStreamBoundary {
    STREAM_BOUNDARY_FRAME,
    STREAM_BOUNDARY_GOP,
    STREAM_BOUNDARY_SINGLE_STREAM
} GstRtpQuicMuxStreamBoundary;

struct _RtpQuicMuxSession
{
  guint session_id;

  /* GList of GstPads */
  GList *rtp_sink_pads;
  GList *rtcp_sink_pads;
  GList *quic_src_pads;
};

struct _RtpQuicMuxStream
{
  GstPad *stream_pad;

  guint64 stream_offset;
  guint counter;
  gboolean frame_cancelled;

  GMutex mutex;
  GCond wait;
};

typedef struct _RtpQuicMuxStream RtpQuicMuxStream;

#define GST_TYPE_RTPQUICMUX (gst_rtp_quic_mux_get_type())
G_DECLARE_FINAL_TYPE (GstRtpQuicMux, gst_rtp_quic_mux,
    GST, RTPQUICMUX, GstElement)

struct _GstRtpQuicMux
{
  GstElement element;

  GstElement *quicmux;

  gint64 rtp_flow_id;
  gint64 rtcp_flow_id;
  GstRtpQuicMuxStreamBoundary stream_boundary;
  guint stream_packing_ratio;
  guint64 uni_stream_type;
  gboolean use_datagrams;
  gboolean add_uni_stream_header;
  GstPad *datagram_pad;
  guint pad_n;

  /*
   * GHashTable <guint> { // SSRCs
   *    GHashTable <guint8> { // Payload type
   *        RtpQuicMuxStream;
   *    }
   * }
   */
  GHashTable *ssrcs;
  /*
   * GHashTable <GstPad> { // src pad
   *   RtpQuicMuxStream;
   * }
   */
  GHashTable *src_pads;

  /*
   * GHashTable <GstPad> { // RTCP sink pad
   *   GstPad; // RTCP src pad
   * }
   */
  GHashTable *rtcp_pads;

  GRecMutex mutex;
  GCond cond;

  guint64 stream_frames_sent;
  guint64 datagrams_sent;
};

typedef struct _GstQuicMux GstQuicMux;

void
gst_rtp_quic_mux_set_quicmux (GstRtpQuicMux *roqmux, GstQuicMux *qmux);

#define PROP_RTPQUICMUX_ENUMS \
  PROP_RTP_FLOW_ID, \
  PROP_RTCP_FLOW_ID, \
  PROP_STREAM_BOUNDARY, \
  PROP_STREAM_PACKING, \
  PROP_UNI_STREAM_TYPE, \
  PROP_USE_DATAGRAM, \
  PROP_USE_UNI_STREAM_HEADER

#define PROP_RTPQUICMUX_ENUM_CASES PROP_RTP_FLOW_ID:\
  case PROP_RTCP_FLOW_ID: \
  case PROP_STREAM_BOUNDARY: \
  case PROP_STREAM_PACKING: \
  case PROP_UNI_STREAM_TYPE: \
  case PROP_USE_DATAGRAM: \
  case PROP_USE_UNI_STREAM_HEADER

#define gst_rtp_quic_mux_install_properties_map(klass) \
  g_object_class_install_property (gobject_class, PROP_RTP_FLOW_ID, \
      g_param_spec_int64 ("rtp-flow-id", "RTP Flow Identifier", \
          "Identifies a stream of RTP packets and allows for multiple " \
          "streams to be multiplexed on a single connection. -1 will result " \
          "in a value being chosen that should be unique across all instances " \
          "of the rtpquicmux element", \
          -1, QUICLIB_VARINT_MAX, -1, G_PARAM_READWRITE)); \
\
  g_object_class_install_property (gobject_class, PROP_RTCP_FLOW_ID, \
      g_param_spec_int64 ("rtcp-flow-id", "RTCP Flow Identifier", \
          "Identifies a stream of RTCP packets and allows for multiple " \
          "streams to be multiplexed on a single connection. -1 will cause " \
          "this property to be set to the value of the RTP flow-id +1.", \
          -1, QUICLIB_VARINT_MAX, -1, G_PARAM_READWRITE)); \
\
  g_object_class_install_property (gobject_class, PROP_STREAM_BOUNDARY, \
      g_param_spec_enum ("stream-boundary", "Stream Boundary", \
          "Specifies where in a stream to split across QUIC stream boundaries", \
          GST_RTP_QUIC_MUX_TYPE_STREAM_BOUNDARY, STREAM_BOUNDARY_SINGLE_STREAM, \
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)); \
\
  g_object_class_install_property (gobject_class, PROP_STREAM_PACKING, \
      g_param_spec_uint ("stream-packing", "Stream packing", \
          "Acts as a multiplier to the stream boundary property, i.e. a value " \
          "of 5 with a GOP stream boundary means 5 GOPs per stream", \
          1, G_MAXUINT, 1, G_PARAM_READWRITE)); \
\
  g_object_class_install_property (gobject_class, PROP_UNI_STREAM_TYPE, \
      g_param_spec_uint64 ("uni-stream-type", \
          "Unidirectional stream header type", "The value of the stream type " \
          "field to add to new streams if use-uni-stream-hdr set", \
          0, QUICLIB_VARINT_MAX, 0, G_PARAM_READWRITE)); \
\
  g_object_class_install_property (gobject_class, PROP_USE_DATAGRAM, \
      g_param_spec_boolean ("use-datagram", "Use datagrams", \
          "Send RT(C)P packets using the QUIC datagram extension. Mutually " \
          "exclusive with use-uni-stream-hdr", FALSE, G_PARAM_READWRITE)); \
\
  g_object_class_install_property (gobject_class, PROP_USE_UNI_STREAM_HEADER, \
      g_param_spec_boolean ("use-uni-stream-hdr", \
          "Use a unidirectional stream header", "Add a unidirectional stream " \
          "header to every new stream. Useful for using with protocols such " \
          "as SIP-over-QUIC. Mutually exclusive with use-datagram", FALSE, \
          G_PARAM_READWRITE));

G_END_DECLS

#endif /* __GST_RTPQUICMUX_H__ */
