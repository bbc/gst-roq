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
 * SECTION:gstroqsinkbin
 * @title: GstRoQSinkBin
 * @short description: A simple bin that implements an RTP-over-QUIC sender
 *
 * This bin wraps the quicsink, quicmux and rtpquicmux elements into a single
 * bin that exposes application/x-rtp src pads to link to RTP sessions and
 * depayloaders.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch videotestsrc ! x264enc ! rtph264pay mtu=4294967295 !
 *  roqsinkbin location="quic://0.0.0.0:443" mode=server
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include "gstroqsinkbin.h"

#include "gstrtpquicmux.h"

#include <gstquicutil.h>
#include <gstquiccommon.h>

GST_DEBUG_CATEGORY_STATIC (gst_roq_sink_bin_debug);
#define GST_CAT_DEFAULT gst_roq_sink_bin_debug

#undef QUICLIB_ALPN_DEFAULT
#define QUICLIB_ALPN_DEFAULT "rtp-mux-quic-05"

enum
{
  PROP_0,
  PROP_QUIC_ENDPOINT_ENUMS,
  PROP_RTPQUICMUX_ENUMS
};

#define ROQ_FLOW_ID_ANY -1
#define ROQ_FLOW_ID_DEFAULT 1

/*
 * Could these also accept application/x-srtp and application/x-srtcp?
 */
/**
 * GstRoQSinkBin!rtp_sink_%u_%u_%u:
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
 * GstRoQSinkBin!rtcp_sink_%u_%u_%u:
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

#define gst_roq_sink_bin_parent_class parent_class
G_DEFINE_TYPE (GstRoQSinkBin, gst_roq_sink_bin, GST_TYPE_BIN);

GST_ELEMENT_REGISTER_DEFINE (roq_sink_bin, "roqsinkbin", GST_RANK_NONE,
    GST_TYPE_ROQ_SINK_BIN);

static void gst_roq_sink_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_roq_sink_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_roq_sink_bin_query (GstElement *parent, GstQuery *query);

#if 0
static gboolean gst_roq_sink_bin_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstFlowReturn gst_roq_sink_bin_rtp_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
static GstFlowReturn gst_roq_sink_bin_rtcp_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
#endif

static GstPad * gst_roq_sink_bin_request_new_pad (GstElement *element,
    GstPadTemplate *templ, const gchar *name, const GstCaps *caps);
static void gst_roq_sink_bin_release_pad (GstElement *element, GstPad *pad);

#if 0
static void gst_roq_sink_bin_rtpquicdemux_pad_added_cb (GstElement * element,
    GstPad * pad, gpointer data);
#endif

/* GObject vmethod implementations */

static void
gst_roq_sink_bin_class_init (GstRoQSinkBinClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_roq_sink_bin_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_roq_sink_bin_get_property);

  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_roq_sink_bin_query);
  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_roq_sink_bin_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_roq_sink_bin_release_pad);

  gst_quiclib_common_install_endpoint_properties (gobject_class);

  gst_rtp_quic_mux_install_properties_map (gobject_class);

  gst_element_class_set_static_metadata (gstelement_class,
         "RTP-over-QUIC sender", "Network/Protocol/Bin/Sink",
         "Send RTP-over-QUIC streams over the network via QUIC transport",
         "Samuel Hurst <sam.hurst@bbc.co.uk>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&rtp_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
        gst_static_pad_template_get (&rtcp_sink_factory));
}

static void
gst_roq_sink_bin_init (GstRoQSinkBin * self)
{
  self->rtpquicmux = NULL;
  self->quicmux = NULL;
  self->quicsink = NULL;

  GST_OBJECT_FLAG_SET (GST_OBJECT (self), GST_ELEMENT_FLAG_SOURCE);
  gst_bin_set_suppressed_flags (GST_BIN (self),
      GST_ELEMENT_FLAG_SOURCE | GST_ELEMENT_FLAG_SINK);

  g_mutex_init (&self->mutex);

  self->rtpquicmux = gst_element_factory_make_full ("rtpquicmux", "flow-id",
      ROQ_FLOW_ID_DEFAULT, NULL);
  if (self->rtpquicmux == NULL) {
    GST_ERROR_OBJECT (self, "Missing required rtpquicmux element");
    return;
  }

  gst_bin_add (GST_BIN (self), self->rtpquicmux);

  self->quicmux = gst_element_factory_make ("quicmux", NULL);
  if (self->quicmux == NULL) {
    GST_ERROR_OBJECT (self, "Missing required quicmux element");
    gst_object_unref (self->rtpquicmux);
    return;
  }

  gst_rtp_quic_mux_set_quicmux (GST_RTPQUICMUX (self->rtpquicmux),
      (GstQuicMux *) self->quicmux);

  gst_bin_add (GST_BIN (self), self->quicmux);

  self->quicsink = gst_element_factory_make ("quicsink", NULL);
  if (self->quicsink == NULL) {
    GST_ERROR_OBJECT (self, "Missing required quicsink element");
    gst_object_unref (self->rtpquicmux);
    gst_object_unref (self->quicmux);
    return;
  }

  gst_bin_add (GST_BIN (self), self->quicsink);

  /*g_signal_connect_object (self->rtpquicdemux, "pad-added",
      G_CALLBACK (gst_roq_sink_bin_rtpquicdemux_pad_added_cb), self, 0);*/

  /*g_warn_if_fail (gst_quic_demux_add_peer ((GstQuicDemux *) self->quicdemux,
      self->rtpquicdemux));*/

  gst_element_link_pads (self->quicmux, "src", self->quicsink, "sink");
}

static void
gst_roq_sink_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRoQSinkBin *self = GST_ROQ_SINK_BIN (object);

  switch (prop_id) {
    case PROP_QUIC_ENDPOINT_ENUM_CASES:
      g_object_set_property (G_OBJECT (self->quicsink), pspec->name, value);
      break;
    case PROP_RTPQUICMUX_ENUM_CASES:
      if (self->rtpquicmux) {
        g_object_set_property (G_OBJECT (self->rtpquicmux), pspec->name, value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_roq_sink_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRoQSinkBin *self = GST_ROQ_SINK_BIN (object);

  switch (prop_id) {
    case PROP_QUIC_ENDPOINT_ENUM_CASES:
      if (self->quicsink) {
        g_object_get_property (G_OBJECT (self->quicsink), pspec->name, value);
      }
      break;
    case PROP_RTPQUICMUX_ENUM_CASES:
      if (self->rtpquicmux) {
        g_object_get_property (G_OBJECT (self->rtpquicmux), pspec->name, value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

static gboolean
gst_roq_sink_bin_query (GstElement *parent, GstQuery *query)
{
  GstRoQSinkBin *self = GST_ROQ_SINK_BIN (parent);

  GST_LOG_OBJECT (self, "Received %s query", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {

  default:
    break;
  }

  return FALSE;
}

static GstPad *
gst_roq_sink_bin_request_new_pad (GstElement *element, GstPadTemplate *templ,
    const gchar *name, const GstCaps *caps)
{
  GstRoQSinkBin *self = GST_ROQ_SINK_BIN (element);
  GList *pad_templates;
  GstPad *ghost_pad;
  GstPad *internal_sink_pad = NULL;

  GST_DEBUG_OBJECT (self, "Trying to request a new %s pad with name %s",
      (templ->direction == GST_PAD_SINK)?("sink"):(
          (templ->direction == GST_PAD_SRC)?("src"):("unknown direction")),
      name);

  g_mutex_lock (&self->mutex);

  /*pad_templates = (GList *) gst_element_factory_get_static_pad_templates (
      GST_ELEMENT_FACTORY (self->rtpquicmux));*/
  pad_templates = gst_element_get_pad_template_list (self->rtpquicmux);

  for (; pad_templates != NULL; pad_templates = g_list_next (pad_templates)) {
    if (gst_caps_is_always_compatible (
        gst_pad_template_get_caps (GST_PAD_TEMPLATE (pad_templates->data)),
        gst_pad_template_get_caps (templ))) {
      internal_sink_pad = gst_element_request_pad (self->rtpquicmux,
          GST_PAD_TEMPLATE (pad_templates->data), NULL, NULL);
    }
  }

  if (internal_sink_pad == NULL) {
    GST_ERROR_OBJECT (self, "Failed to get a sink pad from rtpquicmux");
    return NULL;
  }

  ghost_pad = gst_ghost_pad_new_from_template (name, internal_sink_pad,
      templ);
  if (ghost_pad == NULL) {
    GST_ERROR_OBJECT (self, "Couldn't create new ghost pad with name %s,"
        "connecting to pad %" GST_PTR_FORMAT " with template %" GST_PTR_FORMAT,
        name, internal_sink_pad, templ);
  } else {
    GST_DEBUG_OBJECT (self, "Created new ghost pad with name %s, "
        "connected to pad %" GST_PTR_FORMAT " with template %" GST_PTR_FORMAT,
        name, internal_sink_pad, templ);
  }

  g_mutex_unlock (&self->mutex);

  if (ghost_pad) {
    gst_pad_set_active (ghost_pad, TRUE);
    gst_element_add_pad (GST_ELEMENT (self), ghost_pad);
  }

  gst_object_unref (internal_sink_pad);
  return ghost_pad;
}

static void
gst_roq_sink_bin_release_pad (GstElement *element, GstPad *pad)
{

}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
roq_sink_bin_init (GstPlugin * roq_sink_bin)
{
  /* debug category for filtering log messages
   *
   * exchange the string 'Template roq_sink_bin' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_roq_sink_bin_debug, "roqsinkbin",
      0, "Template roqsinkbin");

  return GST_ELEMENT_REGISTER (roq_sink_bin, roq_sink_bin);
}

/* PACKAGE: this is usually set by meson depending on some _INIT macro
 * in meson.build and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use meson to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "roqsinkbin"
#endif

/* gstreamer looks for this structure to register roq_sink_bins
 *
 * exchange the string 'Template roq_sink_bin' with your roq_sink_bin description
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    roqsinkbin,
    "roqsinkbin",
    roq_sink_bin_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
