# RTP-over-QUIC elements for GStreamer

This repository contains plugins which provide an implementation of
[RTP-over-QUIC](https://datatracker.ietf.org/doc/draft-ietf-avtcore-rtp-over-quic)
for GStreamer.

## Architecture

![A diagram showing the plugin architecture](/docs/GstSipQuic-quic-transport-roq-architecture.png)

These elements are explicitly designed to work with the elements available from
the [Core QUIC Transport elements for GStreamer](https://github.com/bbc/gst-quic-transport)
project. For more information, see that project.

### RTP Stream Mapping

The RTP-over-QUIC mux element has various options to control how RTP frames are
mapped to QUIC streams. The default is for all RTP packets for a given stream
to be sent on the same QUIC stream. However, it is also possible to send RTP
packets on a new stream for each media frame, or even a new stream for each
group of pictures (if the pipeline encoder supports it). This behaviour is
controlled by the `stream-boundary` property on the `rtpquicmux` element and
is also exposed by the `roqsinkbin` element.

The frame- and GOP-per-stream behaviour can be further tweaked by use of the
`stream-packing` property, which allows the user to specify that *n* frames or
GOPs should be sent on each new stream.

It is important to note that the QUIC transport session should be negotiated
with an appropriately-sized value for the `max-stream-data-uni-remote`
transport parameter to carry the data. By default, the `gst-quic-transport`
elements will only allow 128KiB of data on each stream, which may be
insufficient for larger frame sizes. QUIC transport parameter limits are set
by the receiver and are not negotiated in the traditional sense. Currently, the
elements presented here do not attempt to extend stream limits at run time, but
they will increase the maximum number of unidirectional streams at run time
when older streams have been closed.

## Getting started

This project depends on:

- gst-quic-transport
- GStreamer (>=1.20)

If you do not have `gst-quic-transport` installed, this project will built it
as a subproject. The `gst-quic-transport` project additionally depends on the
following:

- GStreamer-plugins-base (>=1.20)
- GLib w/Gio
- ngtcp2
- QuicTLS (OpenSSL)

Details on how to to build ngtcp2 and QuicTLS can be found in the
[`gst-quic-transport` README](https://github.com/bbc/gst-quic-transport/blob/main/README.md).

### Example sending pipeline, QUIC client

The following is an example GStreamer pipeline where a H.264 video source is
carried using RTP-over-QUIC to another host.

The `rtpquicmux` is configured to put each new frame of video on a new logical
QUIC stream, and the `quicsink` element is configured as a QUIC client. The RTP
payloader is configured with the maximum MTU size, so that each frame is
payloaded into a single RTP packet, instead of being packaged for delivery over
UDP.

```
gst-launch-1.0 videotestsrc ! x264enc ! rtph264pay mtu=4294967295 ! rtpquicmux stream-boundary="frame" ! quicmux ! quicsink location="roq://198.51.100.2:4443" mode=client alpn="rtp-mux-quic-07"
```

For convenience, you can instead use the `roqsinkbin` element as shown below
to set up the pipeline of `rtpquicmux`, `quicmux` and `quicsink` elements
for a client acting as an RTP-over-QUIC sender:

```
gst-launch-1.0 videotestsrc ! x264enc ! rtph264pay mtu=4294967295 ! roqsinkbin location="roq://198.51.100.2:4443" mode=client alpn="rtp-mux-quic-07" stream-boundary="frame"
```

### Example receiving pipeline, QUIC server

The following is an example GStreamer pipeline for receiving and playing back
the H.264 video stream created by the pipelines in the example sending pipeline
section above.

The `quicsrc` element is configured as a QUIC server. The public and private
keys are both PEM-encoded.

```
gst-launch-1.0 quicsrc location="roq://0.0.0.0:4443" alpn="rtp-mux-quic-07" mode=server sni="gst-quic.hostname" cert="cert.pem" privkey="key.pem" ! quicdemux ! rtpquicdemux ! application/x-rtp ! rtph264depay ! queue ! decodebin ! xvimagesink
```

For convenience, you can instead use the `roqsrcbin` element as shown below
to set up the pipeline of `quicsrc`, `quicdemux` and `rtpquicdemux` elements
for a server acting as an RTP-over-QUIC receiver:

```
gst-launch-1.0 roqsrcbin location="roq://0.0.0.0:4443" alpn="rtp-mux-quic-07" mode=server sni="gst-quic.hostname" cert="cert.pem" privkey="key.pem" ! application/x-rtp ! rtph264depay ! queue ! decodebin ! xvimagesink
```
