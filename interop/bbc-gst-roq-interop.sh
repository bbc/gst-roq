#!/bin/bash

#
# Copyright 2024 British Broadcasting Corporation - Research and Development
#
# Author: Sam Hurst <sam.hurst@bbc.co.uk>
#
# * Permission is hereby granted, free of charge, to any person obtaining a
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

_hostname=$(hostname -f)

interop_create_certs () {
    openssl req -x509 \
        -newkey rsa:4096 \
        -keyout key.pem \
        -out cert.pem \
        -sha256 \
        -days 30 \
        -nodes \
        -subj "/C=XX/ST=NA/O=NA/OU=GST-RoQ Interop/CN=${_hostname}"
}

interop_ensure_certs () {
    if [ ! -f "key.pem" -a ! -f "cert.pem" ]; then
        echo "Certs don't exist, creating"
        interop_create_certs
    fi

    EXP_DATE=$(cat "cert.pem" | openssl x509 -noout -enddate)
    curr_date=$(date +%s)
    cert_exp_date=$(date -d "${EXP_DATE//notAfter=}" +%s)

    if [ $curr_date -ge $cert_exp_date ]; then
        echo "Certs expired, creating new"
        interop_create_certs
    fi
}

# client or server
endpoint_mode=""
# stream or datagram
payload_mode=""
# send or receive
participant_mode=""

rtp_payload_mtu_stream=4000000000
rtp_payload_mtu_dgram=1370
rtp_payload_type=96
rtp_flow_id=0
roq_alpn="roq-09"
quic_max_stream_data_uni_remote=4000000000000000

video_x_res=480
video_y_res=360
video_framerate=25
video_secs=10

quic_opts=""
gstreamer_pipeline=""

listen_addr="0.0.0.0"
dest_addr="127.0.0.1"
port=8080

if [[ ! -v GST_DEBUG ]]; then
    GST_DEBUG="*:2"
fi
export GST_DEBUG

getopt -T
if [ $? -ne 4 ]; then
    echo "This script requires the Linux enhanced getopt: See https://github.com/util-linux/util-linux" >&2
    exit 1
fi

OPTS=$(getopt -o 'hlcsfdSR' --long 'help,server,client,stream,stream-per-frame,datagram,send,receive' -n "$0" -- "$@")

if [ $? -ne 0 ]; then
    echo "Couldn't parse options" >&2
    exit 1
fi

eval set -- "$OPTS"
unset OPTS

while true; do
    case "$1" in
        '-h'|'--help')
            echo "Usage: ${0} [OPTIONS]"
            echo -e '\t-h: Print help and exit\n'
            echo -e "Endpoint mode:"
            echo -e "\\t-l\t--server\t\tServer mode"
            echo -e "\t-c\t--client\t\tClient mode\n"
            echo -e "Payload mode:"
            echo -e "\t-s\t--stream\t\tStream"
            echo -e "\t-f\t--stream-per-frame\tStream-per-frame"
            echo -e "\t-d\t--datagram\t\tDatagrams\n"
            echo -e "Participant mode:"
            echo -e "\t-S\t--send\t\t\tSender"
            echo -e "\t-R\t--receive\t\tReceiver"
            exit 0
            ;;
        '-l'|'--server')
            if [[ $endpoint_mode != "" ]]; then
                echo "Can't set endpoint mode more than once!"
                exit 2
            fi
            endpoint_mode="server"
            shift
            continue
            ;;
       '-c'|'--client')
            if [[ $endpoint_mode != "" ]]; then
                echo "Can't set endpoint mode more than once!"
                exit 2
            fi
            endpoint_mode="client"
            shift
            continue
            ;;
        '-s'|'--stream')
            if [[ $payload_mode != "" ]]; then
                echo "Cannot set payload mode more than once!"
                exit 2
            fi
            payload_mode="stream"
            shift
            continue
            ;;
        '-f'|'--stream-per-frame')
            if [[ $payload_mode != "" ]]; then
                echo "Cannot set payload mode more than once!"
                exit 2
            fi
            payload_mode="stream-per-frame"
            shift
            continue
            ;;
        '-d'|'--datagram')
            if [[ $payload_mode != "" ]]; then
                echo "Cannot set payload mode more than once!"
                exit 2
            fi
            payload_mode="datagram"
            shift
            continue
            ;;
        '-S'|'--send')
            if [[ $participant_mode != "" ]]; then
                echo "Cannot set participant mode more than once!"
                exit 2
            fi
            participant_mode="send"
            shift
            continue
            ;;
        '-R'|'--receive')
            if [[ $participant_mode != "" ]]; then
                echo "Cannot set participant mode more than once!"
                exit 2
            fi
            participant_mode="receive"
            shift
            continue
            ;;
        '--')
            shift
            break
            ;;
        *)
            echo "Internal error" >&2
            exit 3
            ;;
    esac
done

if [[ $endpoint_mode == "client" ]]; then
    quic_opts="mode=\"${endpoint_mode}\" alpn=\"${roq_alpn}\" location=\"quic://${dest_addr}:${port}\" max-stream-data-uni-remote=${quic_max_stream_data_uni_remote}"

elif [[ $endpoint_mode == "server" ]]; then
    quic_opts="mode=\"${endpoint_mode}\" alpn=\"${roq_alpn}\" location=\"quic://${listen_addr}:${port}\" max-stream-data-uni-remote=${quic_max_stream_data_uni_remote} cert=cert.pem privkey=key.pem"
    interop_ensure_certs
else
    echo "Need to specify client or server mode"
    exit 4
fi

if [[ $payload_mode == "datagram" ]]; then
    quic_opts+=" enable-datagrams=true"
fi

if [[ $participant_mode == "send" ]]; then
    roqmux_opts=""
    mtu=$rtp_payload_mtu_stream
    if [[ $payload_mode == "datagram" ]]; then
        roqmux_opts="use-datagram=true"
        mtu=$rtp_payload_mtu_dgram
    elif [[ $payload_mode == "stream-per-frame" ]]; then
        roqmux_opts="stream-boundary=frame stream-packing=1"
    fi
    pipeline="videotestsrc num-buffers=$((${video_framerate}*${video_secs})) ! videoconvert ! video/x-raw,width=(int)${video_x_res},height=(int)${video_y_res},framerate=${video_framerate}/1 ! vp8enc ! rtpvp8pay mtu=${mtu} ! application/x-rtp,payload=(int)${rtp_payload_type},clock-rate=(int)90000 ! rtpquicmux rtp-flow-id=${rtp_flow_id} ${roqmux_opts} ! quicmux ! quicsink ${quic_opts}"
elif [[ $participant_mode == "receive" ]]; then
    pipeline="quicsrc ${quic_opts} ! quicdemux ! rtpquicdemux rtp-flow-id=${rtp_flow_id} ! application/x-rtp,payload=(int)96,clock-rate=(int)90000,encoding-name=VP8 ! rtpvp8depay ! queue ! decodebin ! queue ! autovideosink"
else
    echo "Need to set the participant mode to --send or --receive"
    exit 4
fi

echo "### GStreamer RTP-over-QUIC Interop ###"


echo "Running GStreamer..."

set -x

gst-launch-1.0 ${pipeline}
