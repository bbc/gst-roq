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
 * SECTION:gstroqsrcbin
 * @title: GstRoQSrcBin
 * @short description: A simple bin that implements an RTP-over-QUIC receiver
 *
 * This bin wraps the quicsrc, quicdemux and rtpquicdemux elements into a single
 * bin that exposes application/x-rtp src pads to link to RTP sessions and
 * depayloaders.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch roqsrcbin location="quic://0.0.0.0:443" mode=server ! fakesink silent=TRUE
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include "gstroqsrcbin.h"

#include <gstquicutil.h>
#include <gstquiccommon.h>

GST_DEBUG_CATEGORY_STATIC (gst_roq_src_bin_debug);
#define GST_CAT_DEFAULT gst_roq_src_bin_debug

#undef QUICLIB_ALPN_DEFAULT
#define QUICLIB_ALPN_DEFAULT "rtp-mux-quic-05"

enum
{
  PROP_0,
  PROP_ROQ_FLOW_ID,
  PROP_QUIC_ENDPOINT_ENUMS
};

#define ROQ_FLOW_ID_ANY -1
#define ROQ_FLOW_ID_DEFAULT ROQ_FLOW_ID_ANY

/*
 * Could these also accept application/x-srtp and application/x-srtcp?
 */
static GstStaticPadTemplate rtp_src_factory = GST_STATIC_PAD_TEMPLATE (
    "recv_rtp_src_%u_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("application/x-rtp")
    );

static GstStaticPadTemplate rtcp_src_factory = GST_STATIC_PAD_TEMPLATE (
    "recv_rtcp_src_%u_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("application/x-rtcp")
    );

#define gst_roq_src_bin_parent_class parent_class
G_DEFINE_TYPE (GstRoQSrcBin, gst_roq_src_bin, GST_TYPE_BIN);

GST_ELEMENT_REGISTER_DEFINE (roq_src_bin, "roqsrcbin", GST_RANK_NONE,
    GST_TYPE_ROQ_SRC_BIN);

static void gst_roq_src_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_roq_src_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_roq_src_bin_query (GstElement *parent, GstQuery *query);

static GstPad * gst_roq_src_bin_request_new_pad_passthrough (
    GstElement *element, GstPadTemplate *templ, const gchar *name,
    const GstCaps *caps);
static void gst_roq_src_bin_release_pad_passthrough (GstElement *element,
    GstPad *pad);

static void gst_roq_src_bin_rtpquicdemux_pad_added_cb (GstElement * element,
    GstPad * pad, gpointer data);


/* GObject vmethod implementations */

static void
gst_roq_src_bin_class_init (GstRoQSrcBinClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_roq_src_bin_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_roq_src_bin_get_property);

  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_roq_src_bin_query);
  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_roq_src_bin_request_new_pad_passthrough);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_roq_src_bin_release_pad_passthrough);

  g_object_class_install_property (gobject_class, PROP_ROQ_FLOW_ID,
      g_param_spec_int64 ("flow-id", "RTP-over-QUIC Flow ID",
          "Identifies the flow-id that this element is responsible for "
          "forwarding to downstream RTP elements. It will also work for RTCP "
          "messages on flow-id + 1. A value of -1 means that the first "
          "observed flow ID will be taken.",
          ROQ_FLOW_ID_ANY, QUICLIB_VARINT_MAX - 1, ROQ_FLOW_ID_DEFAULT,
          G_PARAM_READWRITE));

  gst_quiclib_common_install_endpoint_properties (gobject_class);

  gst_element_class_set_static_metadata (gstelement_class,
         "RTP-over-QUIC receiver", "Network/Protocol/Bin/Src",
         "Receive RTP-over-QUIC streams over the network via QUIC transport",
         "Samuel Hurst <sam.hurst@bbc.co.uk>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&rtcp_src_factory));
}

static void
gst_roq_src_bin_init (GstRoQSrcBin * self)
{
  gboolean rv;

  self->quicsrc = NULL;
  self->quicdemux = NULL;
  self->rtpquicdemux = NULL;

  GST_OBJECT_FLAG_SET (GST_OBJECT (self), GST_ELEMENT_FLAG_SOURCE);
  gst_bin_set_suppressed_flags (GST_BIN (self),
      GST_ELEMENT_FLAG_SOURCE | GST_ELEMENT_FLAG_SINK);

  g_mutex_init (&self->mutex);

  self->quicsrc = gst_element_factory_make ("quicsrc", NULL);
  if (self->quicsrc == NULL) {
    GST_ERROR_OBJECT (self, "Missing required quicsrc element");
    return;
  }

  gst_bin_add (GST_BIN (self), self->quicsrc);

  self->quicdemux = gst_element_factory_make ("quicdemux", NULL);
  if (self->quicdemux == NULL) {
    GST_ERROR_OBJECT (self, "Missing required quicdemux element");
    return;
  }

  self->rtpquicdemux = gst_element_factory_make ("rtpquicdemux", NULL);
  if (self->rtpquicdemux == NULL) {
    GST_ERROR_OBJECT (self, "Missing required rtpquicdemux element");
    return;
  }

  g_signal_connect_object (self->rtpquicdemux, "pad-added",
      G_CALLBACK (gst_roq_src_bin_rtpquicdemux_pad_added_cb), self, 0);

  /*g_warn_if_fail (gst_quic_demux_add_peer ((GstQuicDemux *) self->quicdemux,
      self->rtpquicdemux));*/
  g_signal_emit_by_name (self->quicdemux, "add-peer", self->rtpquicdemux, &rv);
  if (!rv) {
    GST_WARNING_OBJECT (self,
        "Couldn't add rtpquicdemux as a peer of quicdemux");
  }

  gst_bin_add (GST_BIN (self), self->quicdemux);
  gst_bin_add (GST_BIN (self), self->rtpquicdemux);

  gst_element_link_pads (self->quicsrc, "src", self->quicdemux, "sink");
}

static void
gst_roq_src_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (object);

  GST_DEBUG_OBJECT (self, "Setting property %s", pspec->name);

  switch (prop_id) {
    case PROP_QUIC_ENDPOINT_ENUM_CASES:
      if (prop_id == PROP_MAX_STREAM_DATA_UNI_REMOTE) {
        GST_DEBUG_OBJECT (self, "Setting max stream data uni to %lu", g_value_get_uint64 (value));
      }
      g_object_set_property (G_OBJECT (self->quicsrc), pspec->name, value);
      break;
    case PROP_ROQ_FLOW_ID:
      self->flow_id = g_value_get_int64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_roq_src_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (object);

  switch (prop_id) {
    case PROP_QUIC_ENDPOINT_ENUM_CASES:
      g_object_get_property (G_OBJECT (self->quicsrc), pspec->name, value);
      break;
    case PROP_ROQ_FLOW_ID:
      g_value_set_int64 (value, self->flow_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

static gboolean
gst_roq_src_bin_query (GstElement *parent, GstQuery *query)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (parent);

  GST_LOG_OBJECT (self, "Received %s query", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {
  case GST_QUERY_CUSTOM:
    if (!self->rtpquicdemux) {
      self->rtpquicdemux = gst_element_factory_make ("rtpquicdemux", NULL);
      if (self->rtpquicdemux == NULL) {
        GST_ERROR_OBJECT (self, "Missing required rtpquicdemux element");
        return FALSE;
      }

      g_signal_connect_object (self->rtpquicdemux, "pad-added",
          G_CALLBACK (gst_roq_src_bin_rtpquicdemux_pad_added_cb), self, 0);

      gst_bin_add (GST_BIN (self), self->rtpquicdemux);
    }

    return gst_element_query (self->rtpquicdemux, query);
  default:
    break;
  }

  return FALSE;
}

static GstPad * gst_roq_src_bin_request_new_pad_passthrough (
    GstElement *element, GstPadTemplate *templ, const gchar *name,
    const GstCaps *caps)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (element);
  gchar *capsstr = gst_caps_serialize (caps, GST_SERIALIZE_FLAG_NONE);

  g_assert (self->rtpquicdemux);

  GST_TRACE_OBJECT (self, "Passing through pad request to element %s with caps "
      "%s", gst_element_get_name (self->rtpquicdemux), capsstr);

  g_free (capsstr);

  return gst_element_request_pad (self->rtpquicdemux, templ, name, caps);
}

static void gst_roq_src_bin_release_pad_passthrough (GstElement *element,
    GstPad *pad)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (element);

  g_assert (self->rtpquicdemux);

  gst_element_release_request_pad (self->rtpquicdemux, pad);
}

static void
gst_roq_src_bin_rtpquicdemux_pad_added_cb (GstElement * element, GstPad * pad,
    gpointer data)
{
  GstRoQSrcBin *self = GST_ROQ_SRC_BIN (data);
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  GstStructure *s;
  gint pt;
  guint ssrc;
  gchar name[48];
  GstPad *ghost;

  GST_DEBUG_OBJECT (self, "Element %"GST_PTR_FORMAT" added pad %"GST_PTR_FORMAT
      " with caps %"GST_PTR_FORMAT, element, pad, caps);

  if (GST_PAD_DIRECTION (pad) == GST_PAD_SINK) {
    gst_caps_unref (caps);
    return;
  }

  if (!caps) {
    GST_ERROR_OBJECT (self, "Pad with no caps given.");
    return;
  }

  s = gst_caps_get_structure (caps, 0);
  gst_structure_get_int (s, "payload", &pt);
  gst_structure_get_uint (s, "ssrc", &ssrc);
  gst_caps_unref (caps);

  g_snprintf (name, 48, rtp_src_factory.name_template, pt, ssrc);

  g_mutex_lock (&self->mutex);
  ghost = gst_ghost_pad_new (name, pad);
  gst_element_add_pad (GST_ELEMENT (self), ghost);
  g_mutex_unlock (&self->mutex);
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
roq_src_bin_init (GstPlugin * roq_src_bin)
{
  /* debug category for filtering log messages
   *
   * exchange the string 'Template roq_src_bin' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_roq_src_bin_debug, "roqsrcbin",
      0, "Template roqsrcbin");

  return GST_ELEMENT_REGISTER (roq_src_bin, roq_src_bin);
}

/* PACKAGE: this is usually set by meson depending on some _INIT macro
 * in meson.build and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use meson to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "roqsrcbin"
#endif

/* gstreamer looks for this structure to register roq_src_bins
 *
 * exchange the string 'Template roq_src_bin' with your roq_src_bin description
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    roqsrcbin,
    "roqsrcbin",
    roq_src_bin_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
