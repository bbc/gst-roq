# RTP-over-QUIC elements for GStreamer

This repository contains plugins which provide an implementation of
[RTP-over-QUIC](https://datatracker.ietf.org/doc/draft-ietf-avtcore-rtp-over-quic)
for GStreamer.

## Architecture

These elements are explicitly designed to work with the elements available from
the [Core QUIC Transport elements for GStreamer](https://github.com/bbc/gst-quic-transport)
project. For more information, see that project.

## Getting started

This project depends on:

- gst-quic-transport
- GStreamer (>=1.20)

If you do not have `gst-quic-transport` installed, this project will built it
as a subproject.

### Example sending pipeline, QUIC client

The following is an example GStreamer pipeline where a H.264 video source is
carried using RTP-over-QUIC to another host.

The `rtpquicmux` is configured to put each new frame on a new QUIC stream, and
the `quicsink` element is configured as a QUIC client. The RTP payloader is
configured with the maximum MTU size, so that each frame will be payloaded into
a single RTP packet, instead of being packaged for delivery over UDP.

```
gst-launch-1.0 videotestsrc ! x264enc ! rtph264pay mtu=4294967295 ! rtpquicmux stream-boundary="frame" ! quicmux ! quicsink location="roq://198.51.100.2:4443" mode=client alpn="rtp-mux-quic-07"
```

For convenience, you can also use the `roqsinkbin` element as shown below,
 which wraps the `rtpquicmux`, `quicmux` and `quicsink` elements:

```
gst-launch-1.0 videotestsrc ! x264enc ! rtph264pay mtu=4294967295 ! roqsinkbin location="roq://198.51.100.2:4443" mode=client alpn="rtp-mux-quic-07" stream-boundary="frame"
```

### Example receiving pipeline, QUIC server

The following is an example GStreamer pipeline for receiving and playing back
the H.264 video stream created by the pipelines in the Example sending pipeline
section above.

The `quicsrc` element is configured as a QUIC server. The public and private
keys are both PEM-encoded.

```
gst-launch-1.0 quicsrc location="roq://0.0.0.0:4443" alpn="rtp-mux-quic-07" mode=server sni="gst-quic.hostname" cert="cert.pem" privkey="key.pem" ! quicdemux ! rtpquicdemux ! application/x-rtp ! rtph264depay ! queue ! decodebin ! xvimagesink
```

As above, you also yse the `roqsrcbin element as shown below, which wraps the
`quicsrc`, `quicdemux` and `rtpquicdemux` elements:

```
gst-launch-1.0 roqsrcbin location="roq://0.0.0.0:4443" alpn="rtp-mux-quic-07" mode=server sni="gst-quic.hostname" cert="cert.pem" privkey="key.pem" ! application/x-rtp ! rtph264depay ! queue ! decodebin ! xvimagesink
```

