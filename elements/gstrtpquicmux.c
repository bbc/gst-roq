/*
 * Copyright 2023 British Broadcasting Corporation - Research and Development
 *
 * Author: Sam Hurst <sam.hurst@bbc.co.uk>
 *
 * Based on the GStreamer template repository:
 *  https://gitlab.freedesktop.org/gstreamer/gst-template
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
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

/**
 * SECTION:gstrtpquicmux
 * @title: GstRtpQuicMux
 * @short description: Multiplex RTP into RTP-over-QUIC streams and datagrams
 *
 * The rtpquicmux element takes RTP and RTCP packets from upstream elements and
 * maps them onto QUIC streams or into QUIC DATAGRAM packets. It is designed to
 * work with the quicmux element, which then further multiplexes all the QUIC
 * streams and QUIC DATAGRAM frames together to be sent on a QUIC transport
 * connection.
 *
 * When configured to send RTP and RTCP packets over QUIC streams, every RTP and
 * RTCP packet shall be prefaced with an RTP/RTCP payload header. The first
 * buffer for every stream will additionally be prefaced with an RTP-over-QUIC
 * flow identifier. The number of RTP and RTCP packets sent on each stream can
 * be configured using the stream-boundary and stream-packing properties. Each
 * distinct payload type and SSRC shall be sent on unique stream(s).
 *
 * When the stream-boundary property is set to "frame", the rtpquicmux element
 * waits for the last RTP packet for a frame. These are identified using the
 * GST_BUFFER_FLAG_MARKER flag on the received buffer.
 *
 * When the stream-boundary property is set to "gop", the rtpquicmux element
 * waits for the first RTP packet that does not specify the
 * GST_BUFFER_FLAG_DELTA_UNIT flag on the received buffer. The delta unit flag
 * indicates that the buffer requires previous (or future) buffers to decode
 * successfully. IDR frames are sent without this flag.
 *
 * When the stream-boundary property is set to "single", each unique payload
 * type and SSRC will be sent on a single stream.
 *
 * It is recommended that when sending RTP over QUIC streams, that the RTP
 * payloader does not perform any segmenting of individual frames, and instead
 * payloads them as a single large RTP packet. This improves efficiency, as the
 * whole RTP packet shall then be segmented by the QUIC transport layer and is
 * guaranteed to be reassembled in-order at the receiver. This can be performed
 * by setting the mtu property on the RTP payloader (if one exists) to a large
 * enough number that the RTP payloader always delivers a complete frame inside
 * a single packet.
 *
 * With the stream-boundary set to "frame" or "gop", the stream-packing property
 * controls how many frames or GOPs are sent on each individual stream. It
 * effectively works as a multiplier.
 *
 * When configured to send RTP and RTCP packets over QUIC DATAGRAM frames, each
 * RTP and RTCP packet shall be mapped to exactly one QUIC DATAGRAM frame, and
 * prefaced with an RTP-over-QUIC flow identifier.
 *
 * If you want to send some RTP packets over QUIC streams and some over QUIC
 * DATAGRAM frames, you will need to have two instances of rtpquicmux - one
 * configured to send over QUIC streams ( use-datagram = false ) and another
 * configured to send over QUIC DATAGRAM frames ( use-datagram = true ).
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m videotestsrc ! x264enc ! rtph264enc ! rtpquicmux ! quicmux \
 *                  ! quicsink
 * ]|
 * </refsect2>
 */

/*
 * Currently, this element makes the following assumptions:
 *
 * > If an RTCP sink is connected, then:
 *  > The Flow ID for the RTCP packets will be +1 what is given in the flow-id
 *      property.
 *  > If the use-datagram property is false, then all RTCP messages will be
 *      sent on a single stream, until that stream runs out of stream credit,
 *      and then a new stream should be created and RTCP will emit from there,
 *      regardless of the value of the stream-boundary property.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include "gstrtpquicmux.h"
#include <gst-quic-transport/gstquiccommon.h>
/* DEBUGGING ONLY */
#include <gst-quic-transport/gstquicstream.h>
#include <gst-quic-transport/gstquicdatagram.h>
/* END DEBUGGING ONLY */

#include <arpa/inet.h>
#include <stdio.h>

GST_DEBUG_CATEGORY_STATIC (gst_rtp_quic_mux_debug);
#define GST_CAT_DEFAULT gst_rtp_quic_mux_debug

GType
gst_rtp_quic_mux_stream_boundary_get_type (void)
{
  static GType type = 0;
  static const GEnumValue stream_boundary_types[] = {
      {STREAM_BOUNDARY_FRAME, "All RTP packets for a frame on a stream", "frame"},
      {STREAM_BOUNDARY_GOP, "All RTP packets for a GOP on a stream", "gop"},
      {STREAM_BOUNDARY_SINGLE_STREAM, "All RTP packets on a single stream", "single"},
      {0, NULL, NULL}
  };

  if (g_once_init_enter (&type)) {
    GType _type = g_enum_register_static ("GstRtpQuicMuxStreamBoundary",
        stream_boundary_types);
    g_once_init_leave (&type, _type);
  }

  return type;
}

enum
{
  PROP_0,
  PROP_RTPQUICMUX_ENUMS,
  PROP_MAX
};

/**
 * GstRtpQuicMux!rtp_sink_%u_%u_%u:
 *
 * Sink template for receiving RTP packets to send in a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtp_sink_factory =
    GST_STATIC_PAD_TEMPLATE ("rtp_sink_%u_%u_%u",
        GST_PAD_SINK,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS ("application/x-rtp")
        );

/**
 * GstRtpQuicMux!rtcp_sink_%u_%u_%u:
 *
 * Sink template for receiving RTCP packets to send in a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtcp_sink_factory =
    GST_STATIC_PAD_TEMPLATE ("rtcp_sink_%u_%u_%u",
        GST_PAD_SINK,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS ("application/x-rtcp")
        );

static GstStaticPadTemplate quic_stream_src_factory =
    GST_STATIC_PAD_TEMPLATE (":quic_stream_src_%u",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
        GST_STATIC_CAPS (QUICLIB_UNI_STREAM_CAP)
        );

static GstStaticPadTemplate quic_datagram_src_factory =
    GST_STATIC_PAD_TEMPLATE (":quic_datagram_src_%u",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
        GST_STATIC_CAPS (QUICLIB_DATAGRAM_CAP)
        );

#define gst_rtp_quic_mux_parent_class parent_class
G_DEFINE_TYPE (GstRtpQuicMux, gst_rtp_quic_mux, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE (rtp_quic_mux, "rtpquicmux", GST_RANK_NONE,
    GST_TYPE_RTPQUICMUX);

static void gst_rtp_quic_mux_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_rtp_quic_mux_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_rtp_quic_mux_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstFlowReturn gst_rtp_quic_mux_rtp_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
static GstFlowReturn gst_rtp_quic_mux_rtcp_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);

static GstPad * gst_rtp_quic_mux_request_new_pad (GstElement *element,
    GstPadTemplate *templ, const gchar *name, const GstCaps *caps);
static void gst_rtp_quic_mux_release_pad (GstElement *element, GstPad *pad);

void rtp_quic_mux_hash_value_destroy (GHashTable *pts);

void rtp_quic_mux_pad_added_callback (GstElement *self, GstPad *pad,
    gpointer user_data);

/* GObject vmethod implementations */

/* initialize the rtpquicmux's class */
static void
gst_rtp_quic_mux_class_init (GstRtpQuicMuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_get_property);

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_release_pad);

  gst_rtp_quic_mux_install_properties_map (gobject_class);

  gst_element_class_set_static_metadata (gstelement_class,
        "RTP-over-QUIC multiplexer", "Muxer/Network/Protocol",
        "Send data over the network via QUIC transport",
        "Samuel Hurst <sam.hurst@bbc.co.uk>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtcp_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&quic_stream_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&quic_datagram_src_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad callback functions
 * initialize instance structure
 */
static void
gst_rtp_quic_mux_init (GstRtpQuicMux * roqmux)
{
  roqmux->ssrcs = g_hash_table_new_full (g_int_hash, g_int_equal,
      g_free, (GDestroyNotify) rtp_quic_mux_hash_value_destroy);

  roqmux->quicmux = NULL;

  /* TODO: How to make this unique? */
  roqmux->flow_id = (gint64) g_random_int_range (0, 2147483647);
  roqmux->stream_boundary = STREAM_BOUNDARY_SINGLE_STREAM;
  roqmux->stream_packing_ratio = 1;
  roqmux->use_datagrams = FALSE;
  roqmux->datagram_pad = NULL;
  roqmux->pad_n = 0;

  g_rec_mutex_init (&roqmux->mutex);
  g_cond_init (&roqmux->cond);
}

static void
gst_rtp_quic_mux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (object);

  switch (prop_id) {
    case PROP_FLOW_ID:
      roqmux->flow_id = g_value_get_int64 (value);
      if (roqmux->flow_id == -1) {
        roqmux->flow_id = (gint64) g_random_int_range (0, 2147483647);
      }
      break;
    case PROP_STREAM_BOUNDARY:
      roqmux->stream_boundary = g_value_get_enum (value);
      g_assert ((roqmux->stream_boundary >= STREAM_BOUNDARY_FRAME) &&
          (roqmux->stream_boundary <= STREAM_BOUNDARY_SINGLE_STREAM));
      break;
    case PROP_STREAM_PACKING:
      roqmux->stream_packing_ratio = g_value_get_uint (value);
      break;
    case PROP_USE_DATAGRAM:
      roqmux->use_datagrams = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_quic_mux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (object);

  switch (prop_id) {
    case PROP_FLOW_ID:
      g_value_set_int64 (value, roqmux->flow_id);
      break;
    case PROP_STREAM_BOUNDARY:
      g_value_set_enum (value, roqmux->stream_boundary);
      break;
    case PROP_STREAM_PACKING:
      g_value_set_uint (value, roqmux->stream_packing_ratio);
      break;
    case PROP_USE_DATAGRAM:
      g_value_set_boolean (value, roqmux->use_datagrams);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */
enum RtpQuicMuxCapsType {
  CAPS_ERROR = -1,
  CAPS_RTP,
  CAPS_RTCP,
  CAPS_QUIC_BIDI_STREAM,    /** unused **/
  CAPS_QUIC_UNI_STREAM,     /** unused **/
  CAPS_QUIC_DATAGRAM,       /** unused **/
  CAPS_MAX
};

enum RtpQuicMuxCapsType
rtp_quic_mux_get_caps_type (const GstCaps *caps)
{
  GstCaps *test_caps;
  enum RtpQuicMuxCapsType rv = CAPS_ERROR;

  if (caps == NULL) {
    return CAPS_ERROR;
  }

  test_caps = gst_caps_from_string ("application/x-rtp");
  if (gst_caps_can_intersect (caps, test_caps)) {
    rv = CAPS_RTP;
    goto done;
  }

  gst_caps_unref (test_caps);

  test_caps = gst_caps_from_string ("application/x-rtcp");
  if (gst_caps_can_intersect (caps, test_caps)) {
    rv = CAPS_RTCP;
    goto done;
  }

done:
  gst_caps_unref (test_caps);
  return rv;
}

static GstPad *
gst_rtp_quic_mux_request_new_pad (GstElement *element, GstPadTemplate *templ,
    const gchar *name, const GstCaps *caps)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (element);
  GstPad *pad;

  GST_DEBUG_OBJECT (roqmux, "Requested pad with name %s", name);

  pad = gst_pad_new_from_template (templ, name);

  switch (rtp_quic_mux_get_caps_type (templ->caps)) {
  case CAPS_RTP:
    gst_pad_set_chain_function (pad, gst_rtp_quic_mux_rtp_chain);
    break;
  case CAPS_RTCP:
    gst_pad_set_chain_function (pad, gst_rtp_quic_mux_rtcp_chain);
    break;
  default:
    gchar *s = gst_caps_to_string (caps);
    GST_ERROR_OBJECT (roqmux, "Unknown caps type: %s", s);
    g_free (s);
    g_object_unref (pad);
    return NULL;
  }

  gst_pad_set_event_function (pad, gst_rtp_quic_mux_sink_event);

  gst_element_add_pad (element, pad);

  GST_DEBUG_OBJECT (roqmux, "Added pad %p", pad);

  return pad;
}

static void
gst_rtp_quic_mux_release_pad (GstElement *element, GstPad *pad)
{
  GST_DEBUG_OBJECT (GST_RTPQUICMUX (element), "Removing pad %p", pad);

  gst_element_remove_pad (element, pad);
}

/* this function handles sink events */
static gboolean
gst_rtp_quic_mux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstRtpQuicMux *roqmux;
  gboolean ret = FALSE;

  roqmux = GST_RTPQUICMUX (parent);

  GST_LOG_OBJECT (roqmux, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      /* Purposefully do nothing? */
      ret = TRUE;

      break;
    }
    case GST_EVENT_EOS:
      g_rec_mutex_lock (&roqmux->mutex);
      ret = gst_element_send_event (roqmux->quicmux, event);
      g_rec_mutex_unlock (&roqmux->mutex);
      break;
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  gst_event_unref (event);

  return ret;
}

void
rtp_quic_mux_pt_hash_destroy (RtpQuicMuxStream *stream)
{
  GstElement *parent = GST_ELEMENT (gst_pad_get_parent (stream->stream_pad));
  gst_element_remove_pad (parent, stream->stream_pad);
  g_free (stream);
}

static gboolean
rtp_quic_mux_foreach_sticky_event (GstPad *pad, GstEvent **event,
    gpointer user_data)
{
  g_return_val_if_fail (GST_IS_EVENT (*event), FALSE);

  GST_TRACE_OBJECT (GST_RTPQUICMUX (gst_pad_get_parent (pad)),
      "Pushing sticky event of type %s", GST_EVENT_TYPE_NAME (*event));

  if ((*event)->type != GST_EVENT_CAPS) {
    gst_event_ref (*event);
    return gst_pad_push_event ((GstPad *) user_data, *event);
  }
  return TRUE;
}

#define _VARLEN_INT_MAX_62_BIT 0x4000000000000000ULL
#define _VARLEN_INT_MAX_30_BIT 0x40000000
#define _VARLEN_INT_MAX_14_BIT 0x4000
#define _VARLEN_INT_MAX_6_BIT 0x40

#define _VARLEN_INT_62_BIT 0xC0
#define _VARLEN_INT_30_BIT 0x80
#define _VARLEN_INT_14_BIT 0x40
#define _VARLEN_INT_6_BIT 0x00

#ifdef WORDS_BIGENDIAN
#define bswap64(N) (N)
#else /* !WORDS_BIGENDIAN */
#define bswap64(N) \
  ((uint64_t)(ntohl((uint32_t)(N))) << 32 | ntohl((uint32_t)((N) >> 32)))
#endif /* !WORDS_BIGENDIAN */

#if 0
void
rtp_quic_mux_print_buffer (GstRtpQuicMux *mux, GstBuffer *buf)
{
  guint offset = 0;
  guint idx;

  GST_DEBUG_OBJECT (mux, "GstBuffer %p has %u memory blocks",
      buf, gst_buffer_n_memory (buf));

  /*
   * Format:
   *
   * Buffer 0 (4):
   *    00000000 (00000000): e0 a1 b9 f3
   * Buffer 1 (12):
   *    00000000 (00000004): 01 45 aa f1 57 7f 21 00 00 9a be f1
   * Buffer 2 (1388):
   *    00000000 (00000016): a1 e7 ff 00 00 00 00 00 00 00 16 eb eb a0 f7 44
   *    00000016 (00000032): c4 d9 00 11 22 33 44 55 66 77 88 99 aa bb cc dd
   *    00000032 (00000048): (...)
   */

  for (idx = 0; idx < gst_buffer_n_memory (buf); idx++) {
    GstMemory *mem;
    GstMapInfo map;
    guint line_idx;

    mem = gst_buffer_peek_memory (buf, idx);
    gst_memory_map (mem, &map, GST_MAP_READ);

    GST_DEBUG_OBJECT (mux, "Buffer %u (%lu)", idx, map.size);

    for (line_idx = 0; line_idx < (map.size / 16) + 1; line_idx++) {
      gchar bufline[80];
      gint bufline_offset = 0;
      guint data_idx = 0;

      bufline_offset = sprintf (bufline, "%08u (%08u):", line_idx * 16, offset + line_idx * 16);

      for (data_idx = line_idx * 16; data_idx < map.size && data_idx < (line_idx + 1) * 16; data_idx++) {
        guchar top_nibble = (map.data[data_idx] % 0xf0) >> 4;
        guchar bottom_nibble = map.data[data_idx] % 0x0f;
        bufline_offset += sprintf (bufline + bufline_offset, " %c%c",
            (top_nibble > 9)?(top_nibble + 87):(top_nibble + 48),
            (bottom_nibble > 9)?(bottom_nibble + 87):(bottom_nibble + 48));
      }

      GST_DEBUG_OBJECT (mux, "\t%s", bufline);
    }

    offset += map.size;

    gst_memory_unmap (mem, &map);
  }
}
#endif

gboolean
rtp_quic_mux_write_payload_header (GstRtpQuicMux *roqmux, GstBuffer **buf)
{
  gsize buf_len, varlen_len;
  GstMemory *mem;
  GstMapInfo map;

  buf_len = gst_buffer_get_size (*buf);

  varlen_len = gst_quiclib_set_varint (roqmux->flow_id, NULL);
  varlen_len += gst_quiclib_set_varint (buf_len, NULL);

  mem = gst_allocator_alloc (NULL, varlen_len, NULL);
  gst_memory_map (mem, &map, GST_MAP_WRITE);

  varlen_len = gst_quiclib_set_varint (roqmux->flow_id, map.data);
  varlen_len += gst_quiclib_set_varint (buf_len, map.data + varlen_len);

  GST_TRACE_OBJECT (roqmux,
      "Written Flow ID %lu and payload length %lu in %lu byte header",
      roqmux->flow_id, buf_len, varlen_len);

  gst_memory_unmap (mem, &map);

  *buf = gst_buffer_make_writable (*buf);

  gst_buffer_prepend_memory (*buf, mem);

  return TRUE;
}

gboolean
rtp_quic_mux_pts_hash_table_find (gpointer key, gpointer value,
    gpointer user_data)
{
  gboolean rv;
  RtpQuicMuxStream *stream = (RtpQuicMuxStream *) value;
  g_mutex_lock (&stream->mutex);
  rv = stream->stream_pad == (GstPad *) user_data;
  g_mutex_unlock (&stream->mutex);
  return rv;
}

gboolean
rtp_quic_mux_ssrc_hash_table_find (gpointer key, gpointer value,
    gpointer user_data)
{
  GHashTable *pts_ht = (GHashTable *) value;

  return (g_hash_table_find (pts_ht, rtp_quic_mux_pts_hash_table_find,
      user_data) != NULL);
}

void
rtp_quic_mux_pad_linked_callback (GstPad *self, GstPad *peer,
    gpointer user_data)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (user_data);

  g_rec_mutex_lock (&roqmux->mutex);
  roqmux->quicmux = gst_pad_get_parent_element (peer);
  g_rec_mutex_unlock (&roqmux->mutex);
}

GstPad *
rtp_quic_mux_new_uni_src_pad (GstRtpQuicMux *roqmux, GstPad *sinkpad)
{
  GstPad *rv;
  gchar *padname;

  padname = g_strdup_printf (quic_stream_src_factory.name_template,
      roqmux->pad_n++);

  GST_TRACE_OBJECT (roqmux,
      "Requesting new unidirectional stream pad with name %s", padname);

  rv = gst_pad_new_from_static_template (&quic_stream_src_factory, padname);

  g_assert (rv);

  g_free (padname);

  g_rec_mutex_lock (&roqmux->mutex);

  if (roqmux->quicmux == NULL) {
    g_signal_connect (rv, "linked",
          (GCallback) rtp_quic_mux_pad_linked_callback, (gpointer) roqmux);
  }

  g_assert (gst_element_add_pad (GST_ELEMENT (roqmux), rv));
  g_assert (gst_pad_set_active (rv, TRUE));

  if (!gst_pad_is_linked (rv)) {
    g_assert (roqmux->quicmux != NULL);

    if (!gst_element_link_pads (GST_ELEMENT (roqmux), GST_PAD_NAME (rv),
        roqmux->quicmux, NULL)) {
      GST_WARNING_OBJECT (roqmux, "Couldn't link new pad %s to QuicMux %p!",
          GST_PAD_NAME (rv), roqmux->quicmux);
    }
  }

  g_rec_mutex_unlock (&roqmux->mutex);

  gst_pad_sticky_events_foreach (sinkpad, rtp_quic_mux_foreach_sticky_event,
      (gpointer) rv);

  GST_TRACE_OBJECT (roqmux, "Opened new stream");

  return rv;
}

void
rtp_quic_mux_pad_added_callback (GstElement *self, GstPad *pad,
    gpointer user_data)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (self);

  g_rec_mutex_lock (&roqmux->mutex);

  if (!gst_pad_is_linked (pad)) {
    g_assert (roqmux->quicmux != NULL);

    if (!gst_element_link_pads (self, GST_PAD_NAME (pad), roqmux->quicmux, NULL)) {
      GST_WARNING_OBJECT (roqmux, "Couldn't link new pad %s to QuicMux %p!",
          GST_PAD_NAME (pad), roqmux->quicmux);
    }
  } else if (roqmux->quicmux == NULL) {
    roqmux->quicmux = gst_pad_get_parent_element (gst_pad_get_peer (pad));
    g_assert (roqmux->quicmux != NULL);

    GST_TRACE_OBJECT (roqmux, "Stored downstream QuicMux element %p",
        roqmux->quicmux);
  }

  g_rec_mutex_unlock (&roqmux->mutex);
}

const gchar *
rtp_quic_mux_flow_return_as_string (GstFlowReturn fr)
{
  if (fr >= GST_FLOW_CUSTOM_SUCCESS) {
    return "Flow Custom Success";
  }
  if (fr <= GST_FLOW_CUSTOM_ERROR) {
    return "Flow Custom Error";
  }
  switch (fr) {
  case GST_FLOW_OK:
    return "OK";
  case GST_FLOW_NOT_LINKED:
    return "Not Linked";
  case GST_FLOW_FLUSHING:
    return "Flushing";
  case GST_FLOW_EOS:
    return "End Of Stream";
  case GST_FLOW_NOT_NEGOTIATED:
    return "Not negotiated";
  case GST_FLOW_ERROR:
    return "Fatal Error";
  case GST_FLOW_NOT_SUPPORTED:
    return "Operation Not Supported";
  default:
    /* Fall through */
  }
  return "Unknown Flow Return!";
}

static GstFlowReturn
gst_rtp_quic_mux_rtp_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (parent);
  GstFlowReturn rv;
  GstPad *target_pad = NULL;
  guint32 ssrc;
  gint32 payload_type; /* Needs to be for the hash table */
  GHashTable *pts = NULL;
  RtpQuicMuxStream *stream = NULL;

  GST_DEBUG_OBJECT (roqmux, "Received buffer of length %lu bytes",
      gst_buffer_get_size (buf));

  GST_QUICLIB_PRINT_BUFFER (roqmux, buf);

  if (!roqmux->use_datagrams) {
    GstCaps *padcaps;
    gchar *padcapsdbg;
    GList *list;
    guint i;

    padcaps = gst_pad_get_current_caps (pad);

    padcapsdbg = gst_caps_to_string (padcaps);
    GST_DEBUG_OBJECT (roqmux, "Caps: %s", padcapsdbg);
    g_free (padcapsdbg);

    g_warn_if_fail (gst_structure_get_int (
        gst_caps_get_structure (padcaps, 0), "payload", &payload_type));
    g_warn_if_fail (gst_structure_get_uint (
        gst_caps_get_structure (padcaps, 0), "ssrc", &ssrc));

    gst_caps_unref (padcaps);

    list = g_hash_table_get_keys (roqmux->ssrcs);
    GST_TRACE_OBJECT (roqmux, "There are %u SSRCs", g_list_length (list));
    for (i = 0; i < g_list_length (list); i++) {
      GST_TRACE_OBJECT (roqmux, "SSRC %u: %d", i, *((gint *) list->data));
    }
    g_list_free (list);

    if (g_hash_table_lookup_extended (roqmux->ssrcs, &ssrc, NULL,
        (gpointer *) &pts)) {
      list = g_hash_table_get_keys (pts);
      GST_TRACE_OBJECT (roqmux, "There are %u payload types for SSRC %d",
          g_list_length (list), ssrc);
      for (i = 0; i < g_list_length (list); i++) {
        GST_TRACE_OBJECT (roqmux, "PT %u: %d", i, *((gint *) list->data));
      }
      g_list_free (list);

      stream = g_hash_table_lookup (pts, &payload_type);
    }

    if (stream == NULL) {
      guint32 *ssrc_ptr = g_malloc (sizeof (guint32));
      gint32 *pt_ptr = g_malloc (sizeof (gint32));

      *ssrc_ptr = ssrc;
      *pt_ptr = payload_type;

      /* Create QuicMuxStream object */
      stream = g_new0 (RtpQuicMuxStream, 1);
      g_assert (stream);

      g_mutex_init (&stream->mutex);
      g_cond_init (&stream->wait);

      GST_TRACE_OBJECT (roqmux, "New stream for SSRC %u and payload type %u",
          ssrc, payload_type);

      if (g_hash_table_lookup_extended (roqmux->ssrcs, &ssrc, NULL,
          (gpointer *) &pts) == FALSE) {
        pts = g_hash_table_new_full (g_int_hash, g_int_equal,
              g_free, (GDestroyNotify) rtp_quic_mux_pt_hash_destroy);
        g_assert (g_hash_table_insert (roqmux->ssrcs, ssrc_ptr, pts));
      }

      g_assert (g_hash_table_insert (pts, pt_ptr, stream));
    }

    g_mutex_lock (&stream->mutex);

    if (stream->stream_pad == NULL) {
      stream->stream_pad = rtp_quic_mux_new_uni_src_pad (roqmux, pad);
    }

    rtp_quic_mux_write_payload_header (roqmux, &buf);

    if ((roqmux->stream_boundary == STREAM_BOUNDARY_FRAME) &&
        (GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_MARKER)) {
     stream->counter++;
    } else if ((roqmux->stream_boundary == STREAM_BOUNDARY_GOP) &&
        !(GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_DELTA_UNIT)) {
      /* Start of a new GOP */
      if (++stream->counter > roqmux->stream_packing_ratio) {
        GST_DEBUG_OBJECT (roqmux, "Start of new GOP, exceeding limit of %d",
            roqmux->stream_packing_ratio);
        gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
        stream->stream_pad = rtp_quic_mux_new_uni_src_pad (roqmux, pad);
        stream->counter = 0;
      }
    }

    target_pad = stream->stream_pad;

    g_mutex_unlock (&stream->mutex);

    GST_DEBUG_OBJECT (roqmux,
        "Pushing buffer of length %lu bytes on unidirectional stream",
        gst_buffer_get_size (buf));
  } else {
    if (roqmux->datagram_pad == NULL) {
      gchar *padname;

      padname = g_strdup_printf (quic_datagram_src_factory.name_template,
          0);

      roqmux->datagram_pad = gst_pad_new_from_static_template (
          &quic_datagram_src_factory, padname);

      g_assert (roqmux->datagram_pad);

      g_free (padname);

      gst_pad_set_active (roqmux->datagram_pad, TRUE);
      gst_element_add_pad (GST_ELEMENT (roqmux), roqmux->datagram_pad);
    }

    target_pad = roqmux->datagram_pad;

    rtp_quic_mux_write_payload_header (roqmux, &buf);

    GST_DEBUG_OBJECT (roqmux, "Pushing buffer of length %lu in a datagram",
        gst_buffer_get_size (buf));
  }

  if (target_pad == NULL) {
    GST_ERROR_OBJECT (roqmux, "Couldn't find a target pad!");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  rv = gst_pad_push (target_pad, buf);

  if (!roqmux->use_datagrams &&
      roqmux->stream_boundary == STREAM_BOUNDARY_FRAME &&
      ++stream->counter >= roqmux->stream_packing_ratio) {
    GST_DEBUG_OBJECT (roqmux,
        "End of frame, exceeding limit of %d, closing stream",
        roqmux->stream_packing_ratio);
    g_mutex_lock (&stream->mutex);
    gst_pad_set_active (stream->stream_pad, FALSE);
    /* Force unlink? */
    gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
    stream->stream_pad = NULL;
    stream->counter = 0;
    g_mutex_unlock (&stream->mutex);
  }

  GST_DEBUG_OBJECT (roqmux, "Returning %s",
      rtp_quic_mux_flow_return_as_string (rv));

  g_assert (rv >= 0);

  return rv;
}

static GstFlowReturn
gst_rtp_quic_mux_rtcp_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  /*GstRtpQuicMux *roqmux;*/
  GstPad *target_pad = NULL;

  /*roqmux = GST_RTPQUICMUX (parent);*/

  /*
   * TODO: Filter out RTCP messages that are duplicated by QUIC transport
   */

  return gst_pad_push (target_pad, buf);
}

void
rtp_quic_mux_unlink_pad (gpointer data)
{
  GstPad *local = GST_PAD (data);
  GstElement *parent = gst_pad_get_parent_element (local);

  gst_element_remove_pad (parent, local);
}

void
rtp_quic_mux_hash_value_destroy (GHashTable *pts)
{
  g_hash_table_destroy (pts);
}

void
gst_rtp_quic_mux_set_quicmux (GstRtpQuicMux *roqmux, GstQuicMux *qmux)
{
  g_rec_mutex_lock (&roqmux->mutex);
  roqmux->quicmux = GST_ELEMENT (qmux);
  g_rec_mutex_unlock (&roqmux->mutex);
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
rtpquicmux_init (GstPlugin * rtpquicmux)
{
  /* debug category for filtering log messages
   *
   * exchange the string 'Template rtpquicmux' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_rtp_quic_mux_debug, "rtpquicmux",
      0, "Template rtpquicmux");

  return GST_ELEMENT_REGISTER (rtp_quic_mux, rtpquicmux);
}

/* PACKAGE: this is usually set by meson depending on some _INIT macro
 * in meson.build and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use meson to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "rtpquicmux"
#endif

/* gstreamer looks for this structure to register rtpquicmuxs
 *
 * exchange the string 'Template rtpquicmux' with your rtpquicmux description
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtpquicmux,
    "rtpquicmux",
    rtpquicmux_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
