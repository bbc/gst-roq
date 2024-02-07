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
 * SECTION:gstrtpquicdemux
 * @title: GstRtpQuicDemux
 * @short description: Demultiplex RTP-over-QUIC from QUIC streams and datagrams
 *
 * The rtpquicdemux element is designed to work with the quicdemux element to
 * process received RTP and RTCP data from unidirectional QUIC STREAMs and QUIC
 * DATAGRAMs.
 *
 * For streams, the element waits for a "quic-stream-open" query from an
 * upstream element about a new unidirectional stream being opened, and then
 * peeks at the received buffer and check whether the RTP-over-QUIC flow
 * identifier matches the one that is specified by the flow-id property. If the
 * value of the RoQ flow identifier matches the flow-id property, then the query
 * will return true. The upstream rtpdemux element shall then request a new
 * application/quic+stream+uni sink pad and all subsequent buffers received on
 * that pad shall use the same flow identifier. If the value is equal to the
 * flow-id property +1, then it is assumed to be associated RTCP stream for the
 * specified flow-id.
 *
 * For datagrams, the element checks every received buffer for the RoQ flow
 * identifier and matches it with the flow-id property as above.
 *
 * If the flow-id does not match, then the stream query returns false or the
 * datagram frame is dropped.
 *
 * For both stream- and datagram-delivered RTP-over-QUIC frames, the payload
 * type and SSRC will be parsed and the element shall then match that with the
 * caps of a downstream element linked on a src pad. If no matching pad can be
 * found, this shall cause the _chain method to return GST_FLOW_ERROR, which may
 *  cause the GStreamer pipeline to abort.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m quicsrc ! quicdemux ! rtpquicdemux ! rtph264depay ! \
 *                  decodebin ! autovideosink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include <arpa/inet.h>

#include <gst-quic-transport/gstquiccommon.h>
#include <gst-quic-transport/gstquicstream.h>
#include <gst-quic-transport/gstquicdatagram.h>
#include "gstrtpquicdemux.h"

GST_DEBUG_CATEGORY_STATIC (gst_rtp_quic_demux_debug);
#define GST_CAT_DEFAULT gst_rtp_quic_demux_debug

G_DEFINE_TYPE (RtpQuicDemuxStream, rtp_quic_demux_stream, G_TYPE_OBJECT);

static void
rtp_quic_demux_stream_class_init (RtpQuicDemuxStreamClass *klass)
{
}

static void
rtp_quic_demux_stream_init (RtpQuicDemuxStream *stream)
{
  stream->onward_src_pad = NULL;
  stream->expected_payloadlen = 0;
  stream->buf = NULL;
}

enum
{
  PROP_0,
  PROP_FLOW_ID
};

/**
 * GstRtpQuicDemux!rtp_src_%u_%u_%u:
 *
 * Src template for sending RTP packets received from a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtp_sometimes_src_factory =
    GST_STATIC_PAD_TEMPLATE ("rtp_sometimes_src_%u_%u_%u",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
        GST_STATIC_CAPS ("application/x-rtp")
        );

/**
 * GstRtpQuicDemux!rtcp_sink_%u_%u_%u:
 *
 * Src template for sending RTCP packets received from a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtcp_sometimes_src_factory =
    GST_STATIC_PAD_TEMPLATE ("rtcp_somtimes_src_%u_%u_%u",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
        GST_STATIC_CAPS ("application/x-rtcp")
        );

/**
 * GstRtpQuicDemux!rtp_src_%u_%u_%u:
 *
 * Src template for sending RTP packets received from a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtp_request_src_factory =
    GST_STATIC_PAD_TEMPLATE ("rtp_request_src_%u_%u_%u",
        GST_PAD_SRC,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS ("application/x-rtp")
        );

/**
 * GstRtpQuicDemux!rtcp_sink_%u_%u_%u:
 *
 * Src template for sending RTCP packets received from a RoQ session, in the
 * form rtp_sink_<session_idx>_<ssrc>_<payload_type>
 *
 * Since: 1.24
 */
static GstStaticPadTemplate rtcp_request_src_factory =
    GST_STATIC_PAD_TEMPLATE ("rtcp_request_src_%u_%u_%u",
        GST_PAD_SRC,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS ("application/x-rtcp")
        );

/**
 * GstRtpQuicMux!quic_%s_sink_%u:
 *
 * Sink template for QUIC Streams and QUIC Datagram flows.
 */
static GstStaticPadTemplate quic_uni_sink_factory =
    GST_STATIC_PAD_TEMPLATE (":quic_uni_sink_%u",
        GST_PAD_SINK,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS (QUICLIB_UNI_STREAM_CAP)
        );

static GstStaticPadTemplate quic_dgram_sink_factory =
    GST_STATIC_PAD_TEMPLATE (":quic_dgram_sink_%u",
        GST_PAD_SINK,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS (QUICLIB_DATAGRAM_CAP)
        );

#define gst_rtp_quic_demux_parent_class parent_class
G_DEFINE_TYPE (GstRtpQuicDemux, gst_rtp_quic_demux, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE (rtp_quic_demux, "rtpquicdemux", GST_RANK_NONE,
    GST_TYPE_RTPQUICDEMUX);

static void gst_rtp_quic_demux_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_rtp_quic_demux_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static void gst_rtp_quic_demux_finalise (GObject *object);

static GstStateChangeReturn gst_rtp_quic_demux_change_state (GstElement *elem,
    GstStateChange t);

static gboolean gst_rtp_quic_demux_src_event (GstPad *pad, GstObject *parent,
    GstEvent *event);
static gboolean gst_rtp_quic_demux_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstFlowReturn gst_rtp_quic_demux_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);

static gboolean gst_rtp_quic_demux_query (GstElement *parent, GstQuery *query);

static GstPad * gst_rtp_quic_demux_request_new_pad (GstElement *element,
    GstPadTemplate *templ, const gchar *name, const GstCaps *caps);
static void gst_rtp_quic_demux_release_pad (GstElement *element, GstPad *pad);

void rtp_quic_demux_pad_linked (GstPad *self, GstPad *peer,
    gpointer user_data);
void rtp_quic_demux_pad_unlinked (GstPad *self, GstPad *peer,
    gpointer user_data);

void rtp_quic_demux_ssrc_hash_destroy (GHashTable *pts);
void rtp_quic_demux_pt_hash_destroy (RtpQuicDemuxSrc *src);

/* GObject vmethod implementations */

/* initialize the rtpquicdemux's class */
static void
gst_rtp_quic_demux_class_init (GstRtpQuicDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_rtp_quic_demux_set_property;
  gobject_class->get_property = gst_rtp_quic_demux_get_property;
  gobject_class->finalize = gst_rtp_quic_demux_finalise;

  gstelement_class->query = gst_rtp_quic_demux_query;
  gstelement_class->change_state = gst_rtp_quic_demux_change_state;
  gstelement_class->request_new_pad = gst_rtp_quic_demux_request_new_pad;
  gstelement_class->release_pad = gst_rtp_quic_demux_release_pad;

  g_object_class_install_property (gobject_class, PROP_FLOW_ID,
      g_param_spec_int64 ("flow-id", "Flow Identifier",
          "Identifies the flow-id that this element is responsible for "
          "forwarding to downstream RTP elements. It will also work for RTCP "
          "messages on flow-id + 1. A value of -1 means that the first "
          "observed flow ID will be taken.",
          -1, 4611686018427387902, -1, G_PARAM_READWRITE));

  gst_element_class_set_static_metadata (gstelement_class,
        "RTP-over-QUIC demultiplexer", "Demuxer/Network/Protocol",
        "Receive RTP-over-QUIC media data via QUIC transport",
        "Samuel Hurst <sam.hurst@bbc.co.uk>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_sometimes_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_request_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtcp_sometimes_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtcp_request_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&quic_uni_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&quic_dgram_sink_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad callback functions
 * initialize instance structure
 */
static void
gst_rtp_quic_demux_init (GstRtpQuicDemux * roqdemux)
{
  roqdemux->src_ssrcs = g_hash_table_new_full (g_int_hash, g_int_equal,
      g_free, (GDestroyNotify) rtp_quic_demux_ssrc_hash_destroy);
  roqdemux->src_ssrcs_rtcp = g_hash_table_new_full (g_int_hash, g_int_equal,
      g_free, (GDestroyNotify) rtp_quic_demux_ssrc_hash_destroy);
  roqdemux->quic_streams = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      g_free, gst_object_unref);

  roqdemux->flow_id = -1;
  roqdemux->datagram_sink = NULL;

  GST_DEBUG_OBJECT (roqdemux, "RTP QUIC demux initialised");
}

static void
gst_rtp_quic_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (object);

  switch (prop_id) {
    case PROP_FLOW_ID:
      roqdemux->flow_id = g_value_get_int64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_quic_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (object);

  switch (prop_id) {
    case PROP_FLOW_ID:
      g_value_set_int64 (value, roqdemux->flow_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_quic_demux_finalise (GObject *object)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (object);

  if (roqdemux->sink_peer) {
    gboolean rv;

    g_signal_emit_by_name (roqdemux->sink_peer, "remove-peer", object, &rv);

    if (!rv) {
      GST_WARNING_OBJECT (roqdemux, "Failed to remove self from peer object");
    }
  }

  GST_WARNING_OBJECT (roqdemux, "RTP-over-QUIC demux is being finalised!");
}

static GstStateChangeReturn
gst_rtp_quic_demux_change_state (GstElement *elem, GstStateChange t)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (elem);

  GST_TRACE_OBJECT (roqdemux, "Changing state from %s to %s",
      gst_element_state_get_name ((t & 0xf8) >> 3),
      gst_element_state_get_name (t & 0x7));

  GstStateChangeReturn rv =
      GST_ELEMENT_CLASS (parent_class)->change_state (elem, t);

  return rv;
}

/* GstElement vmethod implementations */

static gboolean
gst_rtp_quic_demux_setcaps (GstRtpQuicDemux *roqdemux, GstCaps *caps)
{
  GstStructure *structure;
  guint64 stream_id;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_uint64 (structure, QUICLIB_STREAMID_KEY, &stream_id)) {
    GST_WARNING_OBJECT (roqdemux, "Couldn't get Stream ID from caps");
    return FALSE;
  }
  GST_DEBUG_OBJECT (roqdemux, "Caps has stream ID %lu", stream_id);
  return TRUE;
}

static gboolean
gst_rtp_quic_demux_src_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  GstRtpQuicDemux *roqdemux;
  gboolean ret;

  roqdemux = GST_RTPQUICDEMUX (parent);

  GST_LOG_OBJECT (roqdemux, "Received %s src event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      GstQOSType qos_type;
      gdouble proportion;
      GstClockTimeDiff diff;
      GstClockTime ts;

      gst_event_parse_qos (event, &qos_type, &proportion, &diff, &ts);
      switch (qos_type) {
        case GST_QOS_TYPE_OVERFLOW:
          GST_TRACE_OBJECT (roqdemux, "Received Overflow QoS event with "
              "timestamp %lu, jitter %ld, proportion %f", ts, diff, proportion);
          break;
        case GST_QOS_TYPE_UNDERFLOW:
        {
          GList *pt_tables;
          RtpQuicDemuxSrc *src;
          gboolean done = FALSE;

          GST_FIXME_OBJECT (roqdemux, "Received %sflow QoS event with "
              "timestamp %lu, jitter %ld, proportion %f",
              (qos_type == GST_QOS_TYPE_OVERFLOW)?("over"):("under"),
              ts, diff, proportion);

          pt_tables = g_hash_table_get_values (roqdemux->src_ssrcs);
          for (; pt_tables != NULL; pt_tables = pt_tables->next) {
            GList *srcs =
                g_hash_table_get_values ((GHashTable *) pt_tables->data);
            for (; srcs != NULL; srcs = srcs->next) {
              src = (RtpQuicDemuxSrc *) srcs->data;
              if (src->src == pad) {
                GST_INFO_OBJECT (roqdemux, "Matched pad %p", pad);
                if (diff < 0) {

                }
                src->offset = (GstClockTimeDiff) src->offset + diff;
                done = TRUE;
                pt_tables = NULL;
                break;
              }
            }
            if (pt_tables == NULL) break;
          }

          if (done) {
            GST_INFO_OBJECT (roqdemux, "%screased buffer offset for pad %p by "
                "%ld to %ld", (diff > 0)?("In"):("De"), pad, diff, src->offset);
          } else {
            pt_tables = g_hash_table_get_values (roqdemux->src_ssrcs_rtcp);
            for (; pt_tables != NULL; pt_tables = pt_tables->next) {
              GList *srcs =
                  g_hash_table_get_values ((GHashTable *) pt_tables->data);
              for (; srcs != NULL; srcs = srcs->next) {
                src = (RtpQuicDemuxSrc *) srcs->data;
                if (src->src == pad) {
                  GST_FIXME_OBJECT (roqdemux,
                      "Deal with buffer offsets for RTCP");
                  break;
                }
              }
            }

          }

          break;
        }
        case GST_QOS_TYPE_THROTTLE:
          GST_FIXME_OBJECT (roqdemux, "Received Throttle QoS event with "
              "timestamp %lu, jitter %ld, proportion %f", ts, diff, proportion);
          break;
      }

      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
  }

  return ret;
}

static gboolean
gst_rtp_quic_demux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstRtpQuicDemux *roqdemux;
  gboolean ret;

  roqdemux = GST_RTPQUICDEMUX (parent);

  GST_LOG_OBJECT (roqdemux, "Received %s sink event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
    {
      /* Swallow these, this element makes its own for downstream */
      gst_event_unref (event);
      ret = TRUE;
      break;
    }
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_rtp_quic_demux_setcaps (roqdemux, caps);
      break;
    }
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {

      break;
    }
    case GST_EVENT_EOS:
      gst_element_send_event (roqdemux->sink_peer, event);
      /* no break */
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  /*gst_event_unref (event);*/

  return ret;
}

static gboolean
forward_sticky_events (GstPad *pad, GstEvent **event, gpointer user_data)
{
  g_return_val_if_fail (GST_IS_EVENT (*event), FALSE);

  GST_LOG_OBJECT (GST_RTPQUICDEMUX (gst_pad_get_parent (pad)),
      "Forwarding sticky event type %s", GST_EVENT_TYPE_NAME (*event));

  gst_event_ref (*event);

  return gst_pad_push_event (pad, *event);
}

void rtp_quic_demux_src_pad_linked (GstPad *self, GstPad *peer,
    gpointer user_data)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (gst_pad_get_parent (self));
  gchar *name;
  GstStream *stream;
  GstEvent *ssevent;

  /*
   * The parent of the pad will be a ghost pad in the case of the roqsrcbin
   */
  if (roqdemux->sink_peer == NULL && GST_IS_ELEMENT (gst_pad_get_parent (peer)))
  {
    roqdemux->sink_peer = GST_ELEMENT (gst_pad_get_parent (peer));
    GST_TRACE_OBJECT (roqdemux, "Set sink peer as %p", roqdemux->sink_peer);
  }

  name = gst_pad_get_name (self);

  stream = gst_stream_new (name, gst_pad_get_current_caps (self),
      GST_STREAM_TYPE_UNKNOWN, GST_STREAM_FLAG_NONE);

  ssevent = gst_event_new_stream_start (name);
  gst_event_set_stream (ssevent, stream);
  gst_stream_set_caps (stream, gst_pad_get_current_caps (self));

  gst_pad_push_event (self, ssevent);

  g_free (name);
}

GstPad *
rtp_quic_demux_get_src_pad (GstRtpQuicDemux *roqdemux, guint64 flow_id,
    guint32 ssrc, guint32 pt, GstClockTime *offset)
{
  GHashTable *ssrc_ht;
  GHashTable *pts_ht;
  RtpQuicDemuxSrc *src = NULL;

  GST_TRACE_OBJECT (roqdemux,
      "Looking up SRC pad for flow ID %lu, SSRC %u, payload type %u", flow_id,
      ssrc, pt);

  if (flow_id == roqdemux->flow_id) {
    ssrc_ht = roqdemux->src_ssrcs;
  } else if (flow_id == (roqdemux->flow_id + 1)) {
    ssrc_ht = roqdemux->src_ssrcs_rtcp;
  } else if (roqdemux->flow_id == -1) {
    /*
     * Use g_object_set as this causes a signal to be emitted in case other
     * objects/apps are looking for the flow ID changing.
     */
    if (pt < 128) {
      g_object_set (roqdemux, "flow-id", G_TYPE_INT64, flow_id, NULL);
      ssrc_ht = roqdemux->src_ssrcs;
    } else {
      g_object_set (roqdemux, "flow-id", G_TYPE_INT64, flow_id - 1, NULL);
      ssrc_ht = roqdemux->src_ssrcs_rtcp;
    }
  } else {
    GST_ERROR_OBJECT (roqdemux, "Flow ID %lu does not match the RTP (%lu) "
        "or RTCP (%lu) flows expected by this element", flow_id,
        roqdemux->flow_id, roqdemux->flow_id + 1);
    return NULL;
  }

  if (g_hash_table_lookup_extended (ssrc_ht, &ssrc, NULL,
      (gpointer) &pts_ht) == FALSE) {
    gint *ssrc_ptr = g_new (gint, 1);
    *ssrc_ptr = ssrc;

    pts_ht = g_hash_table_new_full (g_int_hash, g_int_equal, g_free,
        (GDestroyNotify) rtp_quic_demux_pt_hash_destroy);
    g_assert (g_hash_table_insert (ssrc_ht, ssrc_ptr, pts_ht));
  }

  if (g_hash_table_lookup_extended (pts_ht, &pt, NULL,
      (gpointer) &src) == FALSE) {
    gchar *padname;
    gint *pt_ptr;
    GList *pad_list = roqdemux->pending_req_sinks;
    GstCaps *caps;

    src = g_new0 (RtpQuicDemuxSrc, 1);

    if (pt < 128) {
      caps = gst_caps_new_simple ("application/x-rtp", "payload", G_TYPE_INT,
          pt, NULL);
    } else {
      caps = gst_caps_new_simple ("application/x-rtcp", NULL, NULL);
    }

    for (; pad_list != NULL; pad_list = g_list_next (pad_list)) {
      GstPad *pending_sink = GST_PAD (pad_list->data);
      GstCaps *sink_caps;

      if (!gst_pad_is_linked (pending_sink)) {
        continue;
      }

      sink_caps = gst_pad_get_allowed_caps (pending_sink);

      GST_TRACE_OBJECT (roqdemux, "Pad %" GST_PTR_FORMAT " with caps %"
          GST_PTR_FORMAT ", parent %" GST_PTR_FORMAT, pending_sink, sink_caps,
          GST_PAD_PARENT (GST_PAD_PEER (pending_sink)));

      /*if (gst_pad_is_linked (pending_sink) &&
          gst_pad_peer_query_accept_caps (pending_sink, caps)) {*/
      if (gst_caps_can_intersect (sink_caps, caps)) {
        gchar *name;
        GstStream *stream;
        GstEvent *ssevent;

        GST_DEBUG_OBJECT (roqdemux, "Fulfilling request for new src pad with "
            "compatible pad %" GST_PTR_FORMAT " connected to %s from pending "
            "request sink list", pending_sink, GST_ELEMENT_NAME (
                GST_PAD_PARENT (gst_pad_get_peer (pending_sink))));
        src->src = pending_sink;
        roqdemux->pending_req_sinks =
            g_list_remove (roqdemux->pending_req_sinks,
                (gconstpointer) pending_sink);
        gst_caps_unref (sink_caps);

        gst_pad_set_event_function (src->src, gst_rtp_quic_demux_src_event);

        name = gst_pad_get_name (src->src);

        stream = gst_stream_new (name, gst_pad_get_current_caps (src->src),
            GST_STREAM_TYPE_UNKNOWN, GST_STREAM_FLAG_NONE);

        ssevent = gst_event_new_stream_start (name);
        gst_event_set_stream (ssevent, stream);
        gst_stream_set_caps (stream, gst_pad_get_current_caps (src->src));

        gst_pad_push_event (src->src, ssevent);

        gst_pad_sticky_events_foreach (src->src, forward_sticky_events,
            NULL);

        return src->src;
      }
      gst_caps_unref (sink_caps);
    }

    if (src->src == NULL) {
      pt_ptr = g_new (gint, 1);

      *pt_ptr = (gint) pt;

      if (flow_id == (roqdemux->flow_id + 1)) { /* RTCP */
        padname = g_strdup_printf (rtcp_request_src_factory.name_template,
            (guint32) roqdemux->flow_id, ssrc, pt);
        src->src = gst_pad_new_from_static_template (&rtcp_request_src_factory,
            padname);
      } else { /* RTP */
        padname = g_strdup_printf (rtp_sometimes_src_factory.name_template,
            (guint32) roqdemux->flow_id, ssrc, pt);
        src->src = gst_pad_new_from_static_template (&rtp_sometimes_src_factory,
            padname);
      }

      g_signal_connect (src->src, "linked",
          (GCallback) rtp_quic_demux_src_pad_linked, NULL);

      gst_pad_set_event_function (src->src, gst_rtp_quic_demux_src_event);

      g_free (padname);

      gst_element_add_pad (GST_ELEMENT (roqdemux), src->src);

      gst_pad_set_active (src->src, TRUE);

      if (!gst_pad_is_linked (src->src) && roqdemux->sink_peer != NULL) {
        gst_element_link_pads (GST_ELEMENT (roqdemux),
            GST_PAD_NAME (src->src), roqdemux->sink_peer, NULL);
      }

      gst_pad_sticky_events_foreach (src->src, forward_sticky_events,
          NULL);
    }

    if (gst_debug_category_get_threshold (gst_rtp_quic_demux_debug)
        >= GST_LEVEL_DEBUG) {
      GstPad *peer;
      GstElement *peer_parent;
      gchar *parent_name;

      peer = gst_pad_get_peer (src->src);
      if (GST_IS_GHOST_PAD (peer)) {
        peer_parent = gst_pad_get_parent_element (peer);
        parent_name = gst_element_get_name (peer_parent);

        GST_DEBUG_OBJECT (roqdemux, "Added new RT%sP src pad for flow ID %lu, "
            "SSRC %u, PT %u with padname %s linked to element %s",
            (flow_id == roqdemux->flow_id)?(""):("C"), flow_id, ssrc, pt,
            GST_PAD_NAME (src->src), parent_name);
        g_free (parent_name);
        gst_object_unref (peer_parent);
      } else {
        GstGhostPad *gpad = GST_GHOST_PAD (gst_pad_get_parent (peer));

        GST_DEBUG_OBJECT (roqdemux, "Added new RT%sP src pad for flow ID %lu, "
            "SSRC %u, PT %u with padname %s linked to ghost pad %p",
            (flow_id == roqdemux->flow_id)?(""):("C"), flow_id, ssrc, pt,
            GST_PAD_NAME (src->src), gpad);
      }

      gst_object_unref (peer);
    }

    g_hash_table_insert (pts_ht, pt_ptr, (gpointer) src);
  }

  if (offset != NULL) {
    GST_INFO_OBJECT (roqdemux, "Setting stream offset to %" GST_TIME_FORMAT,
        GST_TIME_ARGS (src->offset));
    *offset = src->offset;
  }

  return src->src;
}

/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_rtp_quic_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstRtpQuicDemux *roqdemux;
  GstQuicLibStreamMeta *stream_meta = NULL;
  RtpQuicDemuxStream *stream = NULL;
  GstQuicLibDatagramMeta *datagram_meta = NULL;
  GstPad *target_pad = NULL;
  GstFlowReturn rv;

  roqdemux = GST_RTPQUICDEMUX (parent);

  GST_TRACE_OBJECT (roqdemux, "Received %lu byte buffer with PTS %"
      GST_TIME_FORMAT ", DTS %" GST_TIME_FORMAT, gst_buffer_get_size (buf),
      GST_TIME_ARGS (buf->pts), GST_TIME_ARGS (buf->dts));

  /*GST_QUICLIB_PRINT_BUFFER (roqdemux, buf);*/

  stream_meta = gst_buffer_get_quiclib_stream_meta (buf);
  if (stream_meta) {

    stream = g_hash_table_lookup (roqdemux->quic_streams,
        &stream_meta->stream_id);
    g_return_val_if_fail (stream, GST_FLOW_NOT_LINKED);
    g_return_val_if_fail (GST_IS_PAD (stream->onward_src_pad), GST_FLOW_ERROR);

    /*
     * Zero length buffers with FIN bits set are common if the remote end
     * couldn't set the FIN bit on it's last block, so sends a zero-length
     * STREAM frame with the FIN bit sent.
     */
    if (gst_buffer_get_size (buf) == 0 && stream_meta->final) {
      /*g_hash_table_remove (roqdemux->quic_streams, &stream_meta->stream_id);*/
      if (stream->buf) {
        gst_buffer_unref (buf);
      }
      return GST_FLOW_OK;
    }

    if (stream->buf == NULL) {
      GstMapInfo map;
      guint64 flowid;
      gsize varint_len = 0;

      gst_buffer_map (buf, &map, GST_MAP_READ);

      varint_len = gst_quiclib_get_varint (map.data, &flowid);
      varint_len += gst_quiclib_get_varint (map.data + varint_len,
          &stream->expected_payloadlen);

      gst_buffer_unmap (buf, &map);

      GST_TRACE_OBJECT (roqdemux, "Start of new RTP-over-QUIC frame on stream "
          "%lu with %lu bytes of expected payload length %lu",
          stream_meta->stream_id, gst_buffer_get_size (buf),
          stream->expected_payloadlen);

      stream->buf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL,
          varint_len, -1);

      stream_meta = gst_buffer_get_quiclib_stream_meta (stream->buf);
      stream_meta->offset += varint_len;
      stream_meta->length = stream->expected_payloadlen;

      GST_TRACE_OBJECT (roqdemux, "Adding %" GST_TIME_FORMAT " offset to PTS %"
          GST_TIME_FORMAT " and DTS %" GST_TIME_FORMAT,
          GST_TIME_ARGS (stream->offset), GST_TIME_ARGS (buf->pts),
          GST_TIME_ARGS (buf->dts));

      stream->buf->pts = buf->pts + stream->offset;
      stream->buf->dts = buf->dts + stream->offset;

      g_assert (GST_IS_BUFFER (stream->buf));
    } else {
      /* Continue in-progress buffer */
      gst_buffer_copy_into (stream->buf, buf, GST_BUFFER_COPY_MEMORY, 0, -1);
    }


    GST_TRACE_OBJECT (roqdemux, "Received %sbuffer of length %lu bytes, making "
        "%lu bytes in concat buffer of expected %lu",
        (stream_meta->final)?("final "):(""), gst_buffer_get_size (buf),
        gst_buffer_get_size (stream->buf), stream->expected_payloadlen);

    if (gst_buffer_get_size (stream->buf) < stream->expected_payloadlen) {
      /*
       * If the final flag is set on the stream, then ignore this return and
       * just give all the data we've received so far to the downstream element.
       * The remote end could have ended the stream early in order to meet a
       * delivery target, so the rest of the pipeline will just have to cope
       * with the resulting loss of the rest of this RTP packet.
       */
      if (!stream_meta->final) {
        return GST_FLOW_OK;
      }
    }

    stream_meta->length = gst_buffer_get_size (buf);
    target_pad = stream->onward_src_pad;
    gst_buffer_unref (buf);
    buf = stream->buf;
    stream->buf = NULL;
  } else {
    GstMapInfo map;
    gsize off = 0;
    guint64 flow_id;
    guint32 ssrc;
    guint8 payload_type;

    datagram_meta = gst_buffer_get_quiclib_datagram_meta (buf);
    g_return_val_if_fail (datagram_meta, GST_FLOW_ERROR);

    gst_buffer_map (buf, &map, GST_MAP_READ);

    off = gst_quiclib_get_varint (map.data, &flow_id);
    payload_type = (guint32) map.data[off + 1] & 0x7f;

    if (flow_id == roqdemux->flow_id) { /* RTP */
      memcpy (&ssrc, map.data + off + 4, 4);
    } else if (flow_id == (roqdemux->flow_id + 1)) { /* RTCP */
      memcpy (&ssrc, map.data + off + 8, 4);
    } else {
      return GST_FLOW_NOT_LINKED;
    }

    ssrc = ntohl (ssrc);

    target_pad = rtp_quic_demux_get_src_pad (roqdemux, flow_id, ssrc,
        payload_type, &roqdemux->dg_offset);

    buf->pts += roqdemux->dg_offset;
    buf->dts += roqdemux->dg_offset;
  }

  g_assert (target_pad);
  g_assert (gst_pad_is_linked (target_pad));

  GST_DEBUG_OBJECT (roqdemux, "Pushing buffer of size %lu bytes (consisting of"
      " %u blocks of GstMemory and refcount %d) with PTS %" GST_TIME_FORMAT
      ", DTS %" GST_TIME_FORMAT " on pad %p", gst_buffer_get_size (buf),
      gst_buffer_n_memory (buf), GST_OBJECT_REFCOUNT_VALUE (buf),
      GST_TIME_ARGS (buf->pts), GST_TIME_ARGS (buf->dts), target_pad);

  GST_QUICLIB_PRINT_BUFFER (roqdemux, buf);

  rv = gst_pad_push (target_pad, buf);

  GST_DEBUG_OBJECT (roqdemux, "Push result: %d", rv);

  if (stream && stream_meta->final) {
    g_hash_table_remove (roqdemux->quic_streams, &stream_meta->stream_id);
  }

  return rv;
}

static gboolean
gst_rtp_quic_demux_query (GstElement *parent, GstQuery *query)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (parent);
  gboolean rv = FALSE;

  GST_LOG_OBJECT (roqdemux, "Received %s query", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CUSTOM:
    {
      const GstStructure *s = gst_query_get_structure (query);

      g_return_val_if_fail (s, FALSE);

      if (gst_structure_has_name (s, QUICLIB_STREAM_OPEN)) {
        guint64 stream_id;

        g_warn_if_fail (gst_structure_get_uint64 (s, QUICLIB_STREAMID_KEY,
            &stream_id));

        if (g_hash_table_lookup_extended (roqdemux->quic_streams, &stream_id,
            NULL, NULL) != FALSE) {
          GST_ERROR_OBJECT (roqdemux, "Got " QUICLIB_STREAM_OPEN
              " query for already-opened stream ID %lu", stream_id);
          break;
        }

        if (QUICLIB_STREAM_IS_UNI (stream_id)) {
          guint64 uni_stream_type;
          const GValue *buf_box;
          GstBuffer *peek;
          GstMapInfo map;
          guint64 flow_id;
          guint64 payload_size;
          gsize varint_size;
          guint8 payload_type;
          guint32 ssrc;
          RtpQuicDemuxStream *stream;
          /*
           * Free'd by g_free presented as the key_destroy_func in
           * g_hash_table_new_full
           */
          gint64 *stream_id_ptr = g_new (gint64, 1);

          g_warn_if_fail (gst_structure_get_uint64 (s, "uni-stream-type",
              &uni_stream_type));

          buf_box = gst_structure_get_value (s, "stream-buf-peek");
          peek = GST_BUFFER (g_value_get_pointer (buf_box));

          gst_buffer_map (peek, &map, GST_MAP_READ);

          varint_size = gst_quiclib_get_varint (map.data, &flow_id);
          varint_size += gst_quiclib_get_varint (map.data + varint_size,
              &payload_size);

          payload_type = (guint32) map.data[varint_size + 1] & 0x7f;

          /*
           * Need to do this here, as otherwise we risk reading the wrong SSRC.
           * Use g_object_set as this causes a signal to be emitted in case
           * other objects/apps are looking for the flow ID changing.
           */
          if (roqdemux->flow_id == -1) {
            if (payload_type < 128) { /* RTP */
              g_object_set (roqdemux, "flow-id", flow_id, NULL);
            } else { /* RTCP */
              g_object_set (roqdemux, "flow-id", flow_id - 1, NULL);
            }
          }

          if (flow_id == (roqdemux->flow_id + 1)) { /* RTCP */
            memcpy (&ssrc, map.data + varint_size + 4, 4);
          } else { /* RTP */
            memcpy (&ssrc, map.data + varint_size + 8, 4);
          }
          ssrc = ntohl (ssrc);

          stream = g_object_new (RTPQUICDEMUX_TYPE_STREAM, NULL);
          g_assert (stream);

          stream->onward_src_pad = rtp_quic_demux_get_src_pad (roqdemux,
              flow_id, ssrc, payload_type, &stream->offset);

          g_assert (gst_pad_is_linked (stream->onward_src_pad));

          GST_TRACE_OBJECT (roqdemux, "Adding SRC pad %p for stream ID %lu",
              stream->onward_src_pad, stream_id);

          *stream_id_ptr = (gint64) stream_id;

          g_hash_table_insert (roqdemux->quic_streams, stream_id_ptr, stream);

          gst_buffer_unmap (peek, &map);

          rv = TRUE;
        }
      } else if (gst_structure_has_name (s, QUICLIB_DATAGRAM)) {
        return TRUE;
      }
      break;
    }
  default:
    /* What's the default handler for a direct element query? */
    break;
  }

  return rv;
}

static gboolean
gst_rtp_quic_demux_pad_query (GstPad *pad, GstObject *parent, GstQuery *query)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (parent);

  GST_DEBUG_OBJECT (roqdemux, "Received pad query %" GST_PTR_FORMAT, query);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *temp, *caps, *filt, *tcaps;

      if (gst_pad_get_direction (pad) == GST_PAD_SINK) {
        caps = gst_caps_new_simple (QUICLIB_UNI_STREAM_CAP, NULL, NULL);
        gst_caps_append (caps,
            gst_caps_new_simple (QUICLIB_DATAGRAM_CAP, NULL, NULL));
      } else {
        caps = gst_caps_new_simple ("application/x-rtp", NULL, NULL);
        gst_caps_append (caps,
            gst_caps_new_simple ("application/x-rtcp", NULL, NULL));
      }

      gst_query_parse_caps (query, &filt);

      tcaps = gst_pad_get_pad_template_caps (pad);
      if (tcaps) {
        temp = gst_caps_intersect (caps, tcaps);
        gst_caps_unref (caps);
        gst_caps_unref (tcaps);
        caps = temp;
      }

      if (filt) {
        temp = gst_caps_intersect (caps, filt);
        gst_caps_unref (caps);
        caps = temp;
      }
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, query);
  }

  return FALSE;
}

static GstPad *
gst_rtp_quic_demux_request_new_pad (GstElement *element, GstPadTemplate *templ,
    const gchar *name, const GstCaps *caps)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (element);
  GstPad *rv;

  rv = gst_pad_new_from_template (templ, NULL);

  if (gst_pad_get_direction (rv) == GST_PAD_SINK) {
    gst_pad_set_chain_function (rv, gst_rtp_quic_demux_chain);
    gst_pad_set_event_function (rv, gst_rtp_quic_demux_sink_event);
    gst_pad_set_query_function (rv, gst_rtp_quic_demux_pad_query);
    g_signal_connect (rv, "linked", (GCallback) rtp_quic_demux_pad_linked,
        NULL);
    g_signal_connect (rv, "unlinked", (GCallback) rtp_quic_demux_pad_unlinked,
        NULL);
  } else {
    g_signal_connect (rv, "linked", (GCallback) rtp_quic_demux_pad_linked,
        NULL);

    roqdemux->pending_req_sinks = g_list_append (roqdemux->pending_req_sinks,
        rv);
  }

  gst_element_add_pad (element, rv);

  return rv;
}

static void gst_rtp_quic_demux_release_pad (GstElement *element, GstPad *pad)
{

}

void rtp_quic_demux_pad_linked (GstPad *self, GstPad *peer,
    gpointer user_data)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (gst_pad_get_parent (self));
  gchar *peer_elem = gst_element_get_name (gst_pad_get_parent (peer));

  GST_DEBUG_OBJECT (roqdemux, "Pad %p linked to peer %p (%s)", self, peer,
      peer_elem);

  g_free (peer_elem);
}

void rtp_quic_demux_pad_unlinked (GstPad *self, GstPad *peer,
    gpointer user_data)
{
  GstRtpQuicDemux *roqdemux = GST_RTPQUICDEMUX (gst_pad_get_parent (self));
  GstCaps *caps = gst_pad_get_current_caps (self);
  guint64 stream_id;

  if (caps) {
    g_warn_if_fail (gst_structure_get_uint64 (gst_caps_get_structure (caps, 0),
        QUICLIB_STREAMID_KEY, &stream_id));

    GST_TRACE_OBJECT (roqdemux, "Removing SRC pad %p for stream ID %lu", self,
        stream_id);

    g_warn_if_fail (g_hash_table_remove (roqdemux->quic_streams, &stream_id));
  }

  GST_DEBUG_OBJECT (roqdemux, "Pad %p unlinked from peer %p", self, peer);
}

void
rtp_quic_demux_ssrc_hash_destroy (GHashTable *pts)
{
  g_hash_table_destroy (pts);
}

void
rtp_quic_demux_pt_hash_destroy (RtpQuicDemuxSrc *src)
{
  gst_element_remove_pad (GST_ELEMENT (gst_pad_get_parent (src->src)),
      src->src);
  g_free (src);
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
rtpquicdemux_init (GstPlugin * rtpquicdemux)
{
  /* debug category for filtering log messages
   *
   * exchange the string 'Template rtpquicdemux' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_rtp_quic_demux_debug, "rtpquicdemux",
      0, "Template rtpquicdemux");

  return GST_ELEMENT_REGISTER (rtp_quic_demux, rtpquicdemux);
}

/* PACKAGE: this is usually set by meson depending on some _INIT macro
 * in meson.build and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use meson to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "rtpquicdemux"
#endif

/* gstreamer looks for this structure to register rtpquicdemuxs
 *
 * exchange the string 'Template rtpquicdemux' with your rtpquicdemux description
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtpquicdemux,
    "rtpquicdemux",
    rtpquicdemux_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
