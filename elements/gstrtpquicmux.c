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
#include "gstroqflowidmanager.h"
#include <gstquiccommon.h>

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

const gchar *
_rtp_quic_mux_stream_boundary_as_string (GstRtpQuicMuxStreamBoundary sb)
{
  switch (sb) {
    case STREAM_BOUNDARY_FRAME: return "FRAME";
    case STREAM_BOUNDARY_GOP: return "GOP";
    case STREAM_BOUNDARY_SINGLE_STREAM: return "SINGLE STREAM";
  }
  return "UNKNOWN";
}

enum
{
  PROP_0,
  PROP_RTPQUICMUX_ENUMS,
  PROP_STREAM_FRAMES_SENT,
  PROP_DATAGRAMS_SENT,
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

static void gst_rtp_quic_mux_finalize (GObject *object);
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
void rtp_quic_mux_remove_rtcp_pad (GstPad *pad);

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

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_finalize);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_get_property);

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_quic_mux_release_pad);

  gst_rtp_quic_mux_install_properties_map (gobject_class);

  g_object_class_install_property (gobject_class, PROP_STREAM_FRAMES_SENT,
      g_param_spec_uint64 ("stream-frames-sent", "Number of STREAM frames sent",
          "A counter of the number of STREAM frames sent for a RoQ stream",
          0, G_MAXUINT64, 0, G_PARAM_READABLE));
  
  g_object_class_install_property (gobject_class, PROP_DATAGRAMS_SENT,
      g_param_spec_uint64 ("datagrams-sent", "Number of DATAGRAMs sent",
          "A counter for the number of DATAGRAMs sent for a RoQ stream",
          0, G_MAXUINT64, 0, G_PARAM_READABLE));

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
  roqmux->src_pads = g_hash_table_new (g_direct_hash, g_direct_equal);
  roqmux->rtcp_pads = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) rtp_quic_mux_remove_rtcp_pad);

  roqmux->quicmux = NULL;

  do {
    roqmux->rtp_flow_id = (gint64) g_random_int_range (0, 2147483647);
  } while (!gst_roq_flow_id_manager_new_flow_id (roqmux->rtp_flow_id));
  roqmux->rtcp_flow_id = -1;
  roqmux->stream_boundary = STREAM_BOUNDARY_SINGLE_STREAM;
  roqmux->stream_packing_ratio = 1;
  roqmux->use_datagrams = FALSE;
  roqmux->datagram_pad = NULL;
  roqmux->pad_n = 0;

  g_rec_mutex_init (&roqmux->mutex);
  g_cond_init (&roqmux->cond);
}

static void
gst_rtp_quic_mux_finalize (GObject *object)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (object);

  if (roqmux->quicmux) {
    gst_object_unref (roqmux->quicmux);
    roqmux->quicmux = 0;
  }
}

static void
gst_rtp_quic_mux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (object);

  switch (prop_id) {
    case PROP_RTP_FLOW_ID:
    {
      gint64 flow_id = g_value_get_int64 (value);
      if (flow_id == -1) {
        while (1) {
          flow_id = (gint64) g_random_int_range (0, 2147483647);
          if (!gst_roq_flow_id_manager_flow_id_in_use ((guint64) flow_id))
            break;
        }
      }
      if (gst_roq_flow_id_manager_new_flow_id ((guint64) flow_id)) {
        gst_roq_flow_id_manager_retire_flow_id ((guint64) roqmux->rtp_flow_id);
        if (roqmux->rtcp_flow_id == -1) {
          gst_roq_flow_id_manager_retire_flow_id (
            (guint64) roqmux->rtp_flow_id + 1);
          gst_roq_flow_id_manager_new_flow_id ((guint64) flow_id);
        }
        roqmux->rtp_flow_id = flow_id;
      } else {
        GST_ERROR_OBJECT (roqmux, "Couldn't set RTP Flow ID to %lu as this is "
            "already in use elsewhere!", flow_id);
      }
      break;
    }
    case PROP_RTCP_FLOW_ID:
    {
      gint64 new_flow_id = g_value_get_int64 (value);
      
      if (new_flow_id == -1) {
        if (roqmux->rtp_flow_id == -1) {
          roqmux->rtcp_flow_id = -1;
          break;
        }
        roqmux->rtcp_flow_id = roqmux->rtp_flow_id + 1;
      } else if (gst_roq_flow_id_manager_new_flow_id ((guint64) new_flow_id)) {
        if (roqmux->rtcp_flow_id == -1) {
          gst_roq_flow_id_manager_retire_flow_id (
            (guint64) roqmux->rtp_flow_id + 1);
        } else {
          gst_roq_flow_id_manager_retire_flow_id (
            (guint64) roqmux->rtcp_flow_id);
          gst_roq_flow_id_manager_new_flow_id ((guint64) new_flow_id);
        }
        roqmux->rtcp_flow_id = new_flow_id;
      } else {
        GST_ERROR_OBJECT (roqmux, "Couldn't set RTCP Flow ID to %lu as this is "
            "already in use elsewhere!", new_flow_id);
      }
      GST_FIXME_OBJECT (roqmux, "RTCP Flow ID: %ld", roqmux->rtcp_flow_id);
      break;
    }
    case PROP_STREAM_BOUNDARY:
      roqmux->stream_boundary = g_value_get_enum (value);
      g_assert ((roqmux->stream_boundary >= STREAM_BOUNDARY_FRAME) &&
          (roqmux->stream_boundary <= STREAM_BOUNDARY_SINGLE_STREAM));
      break;
    case PROP_STREAM_PACKING:
      roqmux->stream_packing_ratio = g_value_get_uint (value);
      break;
    case PROP_UNI_STREAM_TYPE:
      roqmux->uni_stream_type = g_value_get_uint64 (value);
      break;
    case PROP_USE_DATAGRAM:
      if (roqmux->add_uni_stream_header) {
        g_error ("Cannot have both use-datagrams and use-uni-stream-hdr set");
      } else {
        roqmux->use_datagrams = g_value_get_boolean (value);
      }
      break;
    case PROP_USE_UNI_STREAM_HEADER:
      if (roqmux->use_datagrams) {
        g_error ("Cannot have both use-datagrams and use-uni-stream-hdr set");
      } else {
        roqmux->add_uni_stream_header = g_value_get_boolean (value);
      }
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
    case PROP_RTP_FLOW_ID:
      g_value_set_int64 (value, roqmux->rtp_flow_id);
      break;
    case PROP_RTCP_FLOW_ID:
      g_value_set_int64 (value, roqmux->rtcp_flow_id);
      break;
    case PROP_STREAM_BOUNDARY:
      g_value_set_enum (value, roqmux->stream_boundary);
      break;
    case PROP_STREAM_PACKING:
      g_value_set_uint (value, roqmux->stream_packing_ratio);
      break;
    case PROP_UNI_STREAM_TYPE:
      g_value_set_uint64 (value, roqmux->uni_stream_type);
      break;
    case PROP_USE_DATAGRAM:
      g_value_set_boolean (value, roqmux->use_datagrams);
      break;
    case PROP_USE_UNI_STREAM_HEADER:
      g_value_set_boolean (value, roqmux->add_uni_stream_header);
      break;
    case PROP_STREAM_FRAMES_SENT:
      g_value_set_uint64 (value, roqmux->stream_frames_sent);
      break;
    case PROP_DATAGRAMS_SENT:
      g_value_set_uint64 (value, roqmux->datagrams_sent);
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

gboolean
_rtp_quic_mux_count_pads (GstElement *element, GstPad *pad, gpointer user_data)
{
  guint *count = (guint *) user_data;
  if (pad != NULL) {
    (*count)++;
  }
  return TRUE;
}

static GstPad *
gst_rtp_quic_mux_request_new_pad (GstElement *element, GstPadTemplate *templ,
    const gchar *name, const GstCaps *caps)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (element);
  gchar *padname = NULL;
  GstPadChainFunction chainfunc;
  GstPad *pad;
  guint padcount = 0;

  switch (rtp_quic_mux_get_caps_type (templ->caps)) {
  case CAPS_RTP:
    chainfunc = gst_rtp_quic_mux_rtp_chain;
    if (name) {
      padname = g_strdup (name);
    } else {
      gst_element_foreach_sink_pad (element, _rtp_quic_mux_count_pads,
          (gpointer) &padcount);
      padname = g_strdup_printf ("rtp_pad%u", padcount);
    }
    break;
  case CAPS_RTCP:
    chainfunc = gst_rtp_quic_mux_rtcp_chain;
    if (name) {
      padname = g_strdup (name);
    } else {
      gst_element_foreach_sink_pad (element, _rtp_quic_mux_count_pads,
          (gpointer) &padcount);
      padname = g_strdup_printf ("rtcp_pad%u", padcount);
    }
    break;
  default:
    gchar *s = gst_caps_to_string (caps);
    GST_ERROR_OBJECT (roqmux, "Unknown caps type: %s", s);
    g_free (s);
    return NULL;
  }

  GST_DEBUG_OBJECT (roqmux, "Creaing new pad with name %s and caps %"
      GST_PTR_FORMAT, padname, templ->caps);

  pad = gst_pad_new_from_template (templ, padname);

  gst_pad_set_chain_function (pad, chainfunc);
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
      if (roqmux->quicmux) {
        ret = gst_element_send_event (roqmux->quicmux, event);
      }
      g_rec_mutex_unlock (&roqmux->mutex);
      break;
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

void
rtp_quic_mux_pt_hash_destroy (RtpQuicMuxStream *stream)
{
  GstElement *parent = GST_ELEMENT (gst_pad_get_parent (stream->stream_pad));
  g_mutex_lock (&stream->mutex);
  g_hash_table_remove (GST_RTPQUICMUX (parent)->src_pads, stream->stream_pad);
  gst_element_remove_pad (parent, stream->stream_pad);
  g_mutex_unlock (&stream->mutex);
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

gboolean
rtp_quic_mux_write_payload_header (GstBuffer **buf, gint64 stream_type,
    gint64 flow_id, gboolean length)
{
  gsize buf_len, varlen_len = 0;
  GstMemory *mem;
  GstMapInfo map;

  buf_len = gst_buffer_get_size (*buf);

  if (stream_type >= 0) {
    varlen_len += gst_quiclib_set_varint ((guint64) stream_type, NULL);
  }
  if (flow_id >= 0) {
    varlen_len += gst_quiclib_set_varint ((guint64) flow_id, NULL);
  }
  if (length) {
    varlen_len += gst_quiclib_set_varint (buf_len, NULL);
  }

  mem = gst_allocator_alloc (NULL, varlen_len, NULL);
  gst_memory_map (mem, &map, GST_MAP_WRITE);

  varlen_len = 0;

  if (stream_type >=0) {
    varlen_len += gst_quiclib_set_varint ((guint64) stream_type, map.data);
  }
  if (flow_id >= 0) {
    varlen_len += gst_quiclib_set_varint ((guint64) flow_id,
        map.data + varlen_len);
  }
  if (length) {
    varlen_len += gst_quiclib_set_varint (buf_len, map.data + varlen_len);
  }

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

void
rtp_quic_mux_pad_unlinked_callback (GstPad * self, GstPad * peer,
    gpointer user_data)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (user_data);
  RtpQuicMuxStream *stream;

  GST_TRACE_OBJECT (roqmux, "Pad %" GST_PTR_FORMAT " unlinked from peer pad %"
      GST_PTR_FORMAT, self, peer);

  if (!g_hash_table_lookup_extended (roqmux->src_pads, (gpointer) self, NULL,
      (gpointer *) &stream)) {
    GST_DEBUG_OBJECT (roqmux, "Couldn't find stream object for pad %"
        GST_PTR_FORMAT ", already closed?", self);
  } else {
    g_mutex_lock (&stream->mutex);
    gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
    g_hash_table_remove (roqmux->src_pads, (gpointer) stream->stream_pad);
    stream->stream_pad = NULL;
    g_mutex_unlock (&stream->mutex);
  }
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
    GstPadTemplate *req_pad_templ, *quicmux_pad_templ;
    GstPad *remote;
    GstPadLinkReturn link_rv;
    g_assert (roqmux->quicmux != NULL);

    req_pad_templ = gst_static_pad_template_get (&quic_stream_src_factory);
    quicmux_pad_templ = gst_element_get_compatible_pad_template (
      roqmux->quicmux, req_pad_templ);

    if (quicmux_pad_templ == NULL) {
      GST_ERROR_OBJECT (roqmux, "Couldn't get compatible pad template from "
          "quicmux %p with local pad template %" GST_PTR_FORMAT,
          roqmux->quicmux, req_pad_templ);
      gst_object_unref (rv);
      return NULL;
    }

    remote = gst_element_request_pad (roqmux->quicmux, quicmux_pad_templ, NULL,
      NULL);

    link_rv = gst_pad_link (rv, remote);

    if (link_rv != GST_PAD_LINK_OK) {
      switch (link_rv) {
        case GST_PAD_LINK_WRONG_HIERARCHY:
          GST_ERROR_OBJECT (roqmux,
            "RTP-over-QUIC mux and QuicMux have different heirarchy!");
          break;
        case GST_PAD_LINK_WAS_LINKED:
          GST_WARNING_OBJECT (roqmux, "Pad %" GST_PTR_FORMAT " already linked",
              remote);
          break;
        case GST_PAD_LINK_WRONG_DIRECTION:
        case GST_PAD_LINK_NOFORMAT:
        case GST_PAD_LINK_NOSCHED:
          g_abort ();
        case GST_PAD_LINK_REFUSED:
          GST_ERROR_OBJECT (roqmux, "Pad %" GST_PTR_FORMAT " refused link",
            remote);
          break;
        default:
          break;
      }

      gst_object_unref (rv);
      gst_object_unref (remote);
      rv = NULL;
      return NULL;
    }
  }

  g_rec_mutex_unlock (&roqmux->mutex);

  g_signal_connect (rv, "unlinked", 
      (GCallback) rtp_quic_mux_pad_unlinked_callback, (gpointer) roqmux);

  gst_pad_sticky_events_foreach (sinkpad, rtp_quic_mux_foreach_sticky_event,
      (gpointer) rv);

  GST_TRACE_OBJECT (roqmux, "Opened new stream with pad %" GST_PTR_FORMAT 
      " linked to pad %" GST_PTR_FORMAT, rv, GST_PAD_PEER (rv));

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

static gboolean
_rtp_quic_mux_open_datagram_pad (GstRtpQuicMux *roqmux, GstPad *sinkpad)
{
  gchar *padname;
  gboolean rv;

  padname = g_strdup_printf (quic_datagram_src_factory.name_template,
      0);

  roqmux->datagram_pad = gst_pad_new_from_static_template (
      &quic_datagram_src_factory, padname);

  g_assert (roqmux->datagram_pad);

  g_free (padname);

  gst_pad_set_active (roqmux->datagram_pad, TRUE);
  rv = gst_element_add_pad (GST_ELEMENT (roqmux), roqmux->datagram_pad);

  if (!rv) return FALSE;

  if (roqmux->quicmux == NULL) {
    roqmux->quicmux = gst_pad_get_parent_element (
      GST_PAD_PEER (roqmux->datagram_pad));
  }

  gst_pad_sticky_events_foreach (sinkpad, rtp_quic_mux_foreach_sticky_event,
      (gpointer) roqmux->datagram_pad);

  return rv;
}

static GstFlowReturn
gst_rtp_quic_mux_rtp_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (parent);
  gsize rtp_frame_len;
  GstFlowReturn rv;
  GstPad *target_pad = NULL;
  guint32 ssrc;
  gint32 payload_type; /* Needs to be for the hash table */
  GHashTable *pts = NULL;
  RtpQuicMuxStream *stream = NULL;

  rtp_frame_len = gst_buffer_get_size (buf);

  GST_DEBUG_OBJECT (roqmux, "Received buffer of length %lu bytes",
      rtp_frame_len);

  if (!roqmux->use_datagrams) {
    GstCaps *padcaps;
    gchar *padcapsdbg;

    padcaps = gst_pad_get_current_caps (pad);

    padcapsdbg = gst_caps_to_string (padcaps);
    GST_DEBUG_OBJECT (roqmux, "Caps: %s", padcapsdbg);
    g_free (padcapsdbg);

    g_warn_if_fail (gst_structure_get_int (
        gst_caps_get_structure (padcaps, 0), "payload", &payload_type));
    g_warn_if_fail (gst_structure_get_uint (
        gst_caps_get_structure (padcaps, 0), "ssrc", &ssrc));

    gst_caps_unref (padcaps);

    if (g_hash_table_lookup_extended (roqmux->ssrcs, &ssrc, NULL,
        (gpointer *) &pts)) {
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

    if (stream->frame_cancelled) {
      if (GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_MARKER) {
        /* Start of a new frame, so start sending again */
        GST_DEBUG_OBJECT (roqmux, "New frame started, sending again");
        stream->frame_cancelled = FALSE;
      } else {
        g_mutex_unlock (&stream->mutex);
        return GST_FLOW_OK;
      }
    }

    if (stream->stream_pad == NULL) {
      stream->stream_pad = rtp_quic_mux_new_uni_src_pad (roqmux, pad);
      g_hash_table_insert (roqmux->src_pads, (gpointer) stream->stream_pad,
          (gpointer) stream);
      stream->stream_offset = 0;
    }

    GST_TRACE_OBJECT (roqmux, "Stream boundary %s, stream packing ratio %u, "
        "stream counter %u, stream offset %lu, buffer flag marker %s, "
        "buffer flag delta unit %s",
        _rtp_quic_mux_stream_boundary_as_string (roqmux->stream_boundary),
         roqmux->stream_packing_ratio, stream->counter, stream->stream_offset,
        (GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_MARKER)?("set"):("not set"),
        (GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_DELTA_UNIT)?("set"):("not set"));

    if ((roqmux->stream_boundary == STREAM_BOUNDARY_FRAME) &&
        (GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_MARKER)) {
     stream->counter++;
    } else if ((roqmux->stream_boundary == STREAM_BOUNDARY_GOP) &&
        !(GST_BUFFER_FLAGS (buf) & GST_BUFFER_FLAG_DELTA_UNIT)) {
      /* Start of a new GOP */
      if (++stream->counter > roqmux->stream_packing_ratio) {
        GST_DEBUG_OBJECT (roqmux, "Start of new GOP, exceeding limit of %d",
            roqmux->stream_packing_ratio);
        g_hash_table_remove (roqmux->src_pads, (gpointer) stream->stream_pad);
        gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
        stream->stream_pad = rtp_quic_mux_new_uni_src_pad (roqmux, pad);
        g_hash_table_insert (roqmux->src_pads, (gpointer) stream->stream_pad,
            (gpointer) stream);
        stream->stream_offset = 0;
        stream->counter = 0;
      }
    }

    if (stream->stream_offset == 0) {
      rtp_quic_mux_write_payload_header (&buf,
          (roqmux->add_uni_stream_header)?(roqmux->uni_stream_type):(-1),
          roqmux->rtp_flow_id, TRUE);
    } else {
      rtp_quic_mux_write_payload_header (&buf, -1, -1, TRUE);
    }

    target_pad = gst_object_ref (stream->stream_pad);

    buf->offset = stream->stream_offset;
    stream->stream_offset += gst_buffer_get_size (buf);

    /* Hold until the end to stop the pad going way while we're using it */
    g_mutex_unlock (&stream->mutex);

    GST_DEBUG_OBJECT (roqmux,
        "Pushing buffer of length %lu bytes on unidirectional stream",
        gst_buffer_get_size (buf));

    roqmux->stream_frames_sent++;
  } else {
    if (roqmux->datagram_pad == NULL) {
      _rtp_quic_mux_open_datagram_pad (roqmux, pad);
    }

    target_pad = gst_object_ref (roqmux->datagram_pad);

    rtp_quic_mux_write_payload_header (&buf, -1, roqmux->rtp_flow_id, FALSE);

    GST_DEBUG_OBJECT (roqmux, "Pushing buffer of length %lu in a datagram",
        gst_buffer_get_size (buf));

    roqmux->datagrams_sent++;
  }

  if (target_pad == NULL) {
    GST_ERROR_OBJECT (roqmux, "Couldn't find a target pad!");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (gst_debug_category_get_threshold (gst_rtp_quic_mux_debug)
      >= GST_LEVEL_DEBUG) {
    GstMapInfo map;
    gsize off = 0;
    guint64 uni_stream_type;
    guint64 flow_id;
    guint64 payload_length;
    gboolean padding;
    gboolean extension_present;
    guint8 cc;
    guint8 pt;
    guint16 seq_num;
    guint32 timestamp;
    guint32 ssrc;

    gst_buffer_map (buf, &map, GST_MAP_READ);
    if (!roqmux->use_datagrams) {
      if (buf->offset == 0) {
        if (roqmux->add_uni_stream_header) {
          off += gst_quiclib_get_varint (map.data + off, &uni_stream_type);
        }
        off += gst_quiclib_get_varint (map.data + off, &flow_id);
      }
      off += gst_quiclib_get_varint (map.data + off, &payload_length);
    } else {
      /* Datagram */
      off += gst_quiclib_get_varint (map.data + off, &flow_id);
      payload_length = gst_buffer_get_size (buf) - off;
    }
    padding = (map.data[off] & 0x20) > 0;
    extension_present = (map.data[off] & 0x10) > 0;
    cc = map.data[off] & 0x0f;
    pt = map.data[off + 1];
    seq_num = (map.data[off + 2] << 8) + (map.data[off + 3]);
    timestamp = (map.data[off + 4] << 24) + (map.data[off + 5] << 16) +
      (map.data[off + 6] << 8) + (map.data[off + 7]);
    ssrc = ntohl ((map.data[off + 8] << 24) + (map.data[off + 9] << 16) +
      (map.data[off + 10] << 8) + (map.data[off + 11]));
    gst_buffer_unmap (buf, &map);

    if (roqmux->use_datagrams) {
      GST_DEBUG_OBJECT (roqmux, "Sending RTP frame of size %lu bytes "
          "(bufsize %lu) on datagram with flow identifier %lu, "
          "marker bit %sset, payload type %u, sequence number %u, "
          "timestamp %u, and ssrc %u. %u CSRCs present. Padding %spresent. "
          "Extension %spresent", payload_length, gst_buffer_get_size (buf),
          flow_id, (pt & 0x80)?(""):("not "), pt & 0x7f, seq_num, timestamp,
          ssrc, cc, (padding)?(""):("not "), (extension_present)?(""):("not "));
    } else if (buf->offset == 0) {
      if (roqmux->add_uni_stream_header) {
        GST_DEBUG_OBJECT (roqmux, "Sending RTP frame of size %lu bytes "
            "(bufsize %lu) on stream with stream type %lu, flow identifier %lu,"
            " marker bit %sset, payload type %u, sequence number %u, "
            "timestamp %u, and ssrc %u. %u CSRCs present. Padding %spresent. "
            "Extension %spresent", payload_length, gst_buffer_get_size (buf),
            uni_stream_type, flow_id, (pt & 0x80)?(""):("not "), pt & 0x7f,
            seq_num, timestamp, ssrc, cc, (padding)?(""):("not "),
            (extension_present)?(""):("not "));
      } else {
        GST_DEBUG_OBJECT (roqmux, "Sending RTP frame of size %lu bytes "
            "(bufsize %lu) on stream, flow identifier %lu, marker bit %sset, "
            "payload type %u, sequence number %u, timestamp %u, and ssrc %u. "
            "%u CSRCs present. Padding %spresent. Extension %spresent",
            payload_length, gst_buffer_get_size (buf), flow_id,
            (pt & 0x80)?(""):("not "), pt & 0x7f, seq_num, timestamp, ssrc,
            cc, (padding)?(""):("not "), (extension_present)?(""):("not "));
      }
    } else {
      GST_DEBUG_OBJECT (roqmux, "Sending RTP frame of size %lu bytes (bufsize "
          "%lu) on stream, marker bit %sset, payload type %u, "
          "sequence number %u, timestamp %u, and ssrc %u. %u CSRCs present. "
          "Padding %spresent. Extension %spresent", payload_length,
          gst_buffer_get_size (buf), (pt & 0x80)?(""):("not "), pt & 0x7f,
          seq_num, timestamp, ssrc, cc, (padding)?(""):("not "),
          (extension_present)?(""):("not "));
    }
  }


  GST_INFO_OBJECT (roqmux, "Pushing buffer %p (size %lu, RTP frame length %lu)"
      "on pad %" GST_PTR_FORMAT, buf, gst_buffer_get_size (buf), rtp_frame_len,
      target_pad);

  rv = gst_pad_push (target_pad, buf);

  gst_object_unref (target_pad);

  if (!roqmux->use_datagrams &&
      roqmux->stream_boundary == STREAM_BOUNDARY_FRAME &&
      ++stream->counter >= roqmux->stream_packing_ratio) {
    GST_DEBUG_OBJECT (roqmux,
        "End of frame, exceeding limit of %d, closing stream",
        roqmux->stream_packing_ratio);
    g_mutex_lock (&stream->mutex);
    if (stream->stream_pad) {
      gst_pad_set_active (stream->stream_pad, FALSE);
      /* Force unlink? */
      g_hash_table_remove (roqmux->src_pads, (gpointer) stream->stream_pad);
      gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
      stream->stream_pad = NULL;
    }
    stream->counter = 0;
    g_mutex_unlock (&stream->mutex);
  }

  if (rv == GST_FLOW_QUIC_STREAM_CLOSED) {
    /*
     * According to rtp-over-quic-09:
     *
     ** STOP_SENDING is not a request to the sender to stop sending RTP media,
     ** only an indication that a RoQ receiver stopped reading the QUIC stream
     ** being used. This can mean that the RoQ receiver is unable to make use of
     ** the media frames being received because they are "too old" to be used. A
     ** sender with additional media frames to send can continue sending them on
     ** another QUIC stream. Alternatively, new media frames can be sent as QUIC
     ** datagrams (see Section 5.3). In either case, a RoQ sender resuming 
     ** operation after receiving STOP_SENDING can continue starting with the
     ** newest media frames available for sending. This allows a RoQ receiver to
     ** "fast forward" to media frames that are "new enough" to be used.
     **
     ** Any media frame that has already been sent on the QUIC stream that
     ** received the STOP_SENDING frame, MUST NOT be sent again on the new QUIC
     ** stream(s) or DATAGRAMs.
     */

    g_assert (!roqmux->use_datagrams);

    GST_DEBUG_OBJECT (roqmux, "Stream closed, cancelling frame");

    g_mutex_lock (&stream->mutex);
    stream->frame_cancelled = TRUE;
    if (stream->stream_pad) {
      g_hash_table_remove (roqmux->src_pads, (gpointer) stream->stream_pad);
      gst_element_remove_pad (GST_ELEMENT (roqmux), stream->stream_pad);
      stream->stream_pad = NULL;
    }
    stream->counter = 0;
    g_mutex_unlock (&stream->mutex);

    rv = GST_FLOW_OK;
  } else if (rv == GST_FLOW_QUIC_BLOCKED) {
    GST_FIXME_OBJECT (roqmux,
        "What to do when the QUIC connection/stream is blocked?");
  }

  GST_DEBUG_OBJECT (roqmux, "Returning %s",
      rtp_quic_mux_flow_return_as_string (rv));

  g_assert (rv >= 0);

  return rv;
}

static GstFlowReturn
gst_rtp_quic_mux_rtcp_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstRtpQuicMux *roqmux;
  GstPad *target_pad = NULL;

  roqmux = GST_RTPQUICMUX (parent);

  if (roqmux->rtcp_flow_id == -1) {
    roqmux->rtcp_flow_id = roqmux->rtp_flow_id + 1;
  }

  if (roqmux->use_datagrams) {
    if (roqmux->datagram_pad == NULL) {
      _rtp_quic_mux_open_datagram_pad (roqmux, pad);
    }

    target_pad = gst_object_ref (roqmux->datagram_pad);

    rtp_quic_mux_write_payload_header (&buf, -1, roqmux->rtcp_flow_id, FALSE);

    GST_DEBUG_OBJECT (roqmux, "Pushing buffer of length %lu in a datagram",
        gst_buffer_get_size (buf));

    roqmux->datagrams_sent++;
  } else {
    if (!g_hash_table_lookup_extended (roqmux->rtcp_pads, (gconstpointer) pad,
        NULL, (gpointer *) &target_pad)) {
      GST_DEBUG_OBJECT (roqmux, "Opening new RTCP stream for RTCP pad %"
          GST_PTR_FORMAT, pad);
      
      target_pad = rtp_quic_mux_new_uni_src_pad (roqmux, pad);

      if (target_pad == NULL) {
        GST_ERROR_OBJECT (roqmux,
            "Couldn't open new unidirectional stream for RTCP");
        return GST_FLOW_NOT_LINKED;
      }

      g_hash_table_insert (roqmux->rtcp_pads, (gpointer) pad,
          (gpointer) target_pad);

      rtp_quic_mux_write_payload_header (&buf,
          (roqmux->add_uni_stream_header)?(roqmux->uni_stream_type):(-1),
          roqmux->rtcp_flow_id, TRUE);
    } else {
      rtp_quic_mux_write_payload_header (&buf, -1, -1, TRUE);
    }

    roqmux->stream_frames_sent++;
  }

  /*
   * TODO: Filter out RTCP messages that are duplicated by QUIC transport
   */

  if (gst_debug_category_get_threshold (gst_rtp_quic_mux_debug)
      >= GST_LEVEL_DEBUG) {
    GstMapInfo map;
    gsize off = 0;
    guint64 uni_stream_type;
    guint64 flow_id;
    guint8 rc;
    guint8 pt;
    guint16 length;
    guint32 ssrc;

    gst_buffer_map (buf, &map, GST_MAP_READ);
    if (!roqmux->use_datagrams && roqmux->add_uni_stream_header) {
      off += gst_quiclib_get_varint (map.data + off, &uni_stream_type);
    }
    off += gst_quiclib_get_varint (map.data + off, &flow_id);
    rc = map.data[off] & 0x1f;
    pt = map.data[off + 1];
    length = (map.data[off + 2] << 8) + (map.data[off + 3]);
    ssrc = (map.data[off + 4] << 24) + (map.data[off + 5] << 16) +
      (map.data[off+6] << 8) + (map.data[off+7]);
    gst_buffer_unmap (buf, &map);

    if (!roqmux->use_datagrams && roqmux->add_uni_stream_header) {
      GST_DEBUG_OBJECT (roqmux, "Sending RTCP frame of size %lu bytes on "
          "stream with stream type %lu, flow identifier %lu, record count %u, "
          "payload type %u, length %u and ssrc %u",
          gst_buffer_get_size (buf), uni_stream_type, flow_id, rc, pt, length,
          ssrc);
    } else {
      GST_DEBUG_OBJECT (roqmux, "Sending RTCP frame of size %lu bytes on "
          "%s, flow identifier %lu, record count %u, payload type %u, length "
          "%u and ssrc %u", gst_buffer_get_size (buf),
          (roqmux->use_datagrams)?("datagram"):("stream"), flow_id, rc, pt,
          length, ssrc);
    }
  }

  return gst_pad_push (target_pad, buf);
}

void rtp_quic_mux_remove_rtcp_pad (GstPad *pad)
{
  GstRtpQuicMux *roqmux = GST_RTPQUICMUX (GST_PAD_PARENT (pad));

  gst_element_remove_pad (GST_ELEMENT (roqmux), pad);
}

void
rtp_quic_mux_unlink_pad (gpointer data)
{
  GstPad *local = GST_PAD (data);
  GstElement *parent = gst_pad_get_parent_element (local);

  g_hash_table_remove (GST_RTPQUICMUX (parent)->src_pads, (gpointer) local);
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
