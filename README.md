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
