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

check_gst_element () {
    element_name=$1

    #
    # gst-inspect-1.0's --exists flag is broken for plugins in GST_PLUGIN_PATH:
    #   https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/3246
    #
    gst-inspect-1.0 ${element_name} &> /dev/null
    if [[ $? -ne 0 ]]; then
        echo "Couldn't find element ${element_name}, searching for it..."
        search_path=$(find ${PWD}/../ -name "libgst${element_name}.so" -printf %h -quit)
        if [[ $? -ne 0 ]]; then
            echo "Couldn't find element ${element_name} in the project directory - has it been built?"
            echo "Alternatively, provide the directory where it is installed in the GST_PLUGIN_PATH environment variable."
            exit -1
        fi
        export GST_PLUGIN_PATH=${search_path}:${GST_PLUGIN_PATH}
    fi
}

# client or server
endpoint_mode=""
# stream or datagram
payload_mode=""
# sendi, receive or bidi
participant_mode=""

multi_stream=FALSE
rtcp=TRUE

sending_video_pt=96
sending_audio_pt=97
receiving_video_pt=$sending_video_pt
receiving_audio_pt=$sending_audio_pt
sending_video_flow_id=0
sending_audio_flow_id=2
receiving_video_flow_id=$sending_video_flow_id
receiving_audio_flow_id=$sending_audio_flow_id

sending_ssrc=123456
receiving_ssrc=123456

rtp_payload_mtu_stream=4000000000
rtp_payload_mtu_dgram=1370
rtp_payload_type=96
rtp_flow_id=0
roq_alpn="roq-12"
quic_max_stream_data_uni_remote=4000000000000000

mtu=0

multi_stream=FALSE
rtcp=FALSE

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

OPTS=$(getopt -o 'hlcsfdSRBMC' --long 'help,server,client,stream,stream-per-frame,datagram,send,receive,bidi,multi,rtcp' -n "$0" -- "$@")

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
            echo -e "\t-B\t--bidi\t\t\tBidirectional\n"
            echo -e "Multiple streams:"
            echo -e "\t-M\t--multi\t\t\tMultiple stream mode (1x video, 1x audio)\n"
            echo -e "RTCP:"
            echo -e "\t-C\t--rtcp\t\t\tInclude RTCP Signalling"
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
            mtu=${rtp_payload_mtu_stream}
            shift
            continue
            ;;
        '-f'|'--stream-per-frame')
            if [[ $payload_mode != "" ]]; then
                echo "Cannot set payload mode more than once!"
                exit 2
            fi
            payload_mode="stream-per-frame"
            mtu=${rtp_payload_mtu_stream}
            shift
            continue
            ;;
        '-d'|'--datagram')
            if [[ $payload_mode != "" ]]; then
                echo "Cannot set payload mode more than once!"
                exit 2
            fi
            payload_mode="datagram"
            mtu=${rtp_payload_mtu_dgram}
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
            participant_mode="recv"
            shift
            continue
            ;;
        '-B'|'--bidi')
            if [[ $participant_mode != "" ]]; then
                echo "Cannot set participant mode more than once!"
                exit 2
            fi
            participant_mode="bidi"
            shift
            continue
            ;;
        '-M'|'--multi')
            multi_stream=TRUE
            shift
            continue
            ;;
        '-C'|'--rtcp')
            rtcp=TRUE
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

#
# RTCP only carried in datagrams for timing/loss reasons
#
if [[ $payload_mode == "datagram" || $rtcp == TRUE ]]; then
    quic_opts+=" enable-datagrams=true"
fi

rtpbin=""
recv_pipeline=""
send_pipeline=""

if [[ $rtcp == TRUE ]]; then
    rtpbin="rtpbin name=rbin "
fi

if [[ $participant_mode == "recv" ]]; then
    recv_pipeline="quicsrc ${quic_opts} ! quicdemux name=qd "
    if [[ $rtcp == TRUE ]]; then
        send_pipeline="quicmux name=qm ! quicsink ${quic_opts} "
    fi
elif [[ $participant_mode == "send" ]]; then
    send_pipeline="quicmux name=qm ! quicsink ${quic_opts} "
    if [[ $rtcp == TRUE ]]; then
        recv_pipeline="quicsrc ${quic_opts} ! quicdemux name=qd "
    fi
elif [[ $participant_mode == "bidi" ]]; then
    recv_pipeline="quicsrc ${quic_opts} ! quicdemux name=qd "
    send_pipeline="quicmux name=qm ! quicsink ${quic_opts} "
    if [[ $endpoint_mode == "client" ]]; then
        receiving_video_pt=98
        receiving_audio_pt=99
        receiving_video_flow_id=4
        receiving_audio_flow_id=6
        receiving_ssrc=123457
    else
        sending_video_pt=98
        sending_audio_pt=99
        sending_video_flow_id=4
        sending_audio_flow_id=6
        sending_ssrc=123457
    fi
else
    echo "Need to set the participant mode to --send or --receive"
    exit 4
fi

if [[ $participant_mode == "recv" || $participant_mode == "bidi" ]]; then
    recv_pipeline+=" qd. ! rtpquicdemux rtp-flow-id=${receiving_video_flow_id} ! application/x-rtp,payload=${receiving_video_pt},clock-rate=90000,encoding-name=VP8,media=video ! "
    if [[ $rtcp == TRUE ]]; then
        recv_pipeline+="rbin.recv_rtp_sink_0 qd. ! rtpquicdemux rtcp-flow-id=$((${receiving_video_flow_id}+1)) ! rbin.recv_rtcp_sink_0 rbin.send_rtcp_src_0 ! rtpquicmux rtcp-flow-id=$((${receiving_video_flow_id}+1)) use-datagram=TRUE ! qm. rbin.recv_rtp_src_0_${receiving_ssrc}_${receiving_video_pt} ! "
    fi
    recv_pipeline+="rtpvp8depay ! queue2 ! decodebin ! queue2 ! autovideosink "

    if [[ $multi_stream == TRUE ]]; then
        recv_pipeline+=" qd. ! rtpquicdemux rtp-flow-id=${receiving_audio_flow_id} ! application/x-rtp,payload=${receiving_audio_pt},clock-rate=48000,encoding-name=OPUS,media=audio ! "
        if [[ $rtcp == TRUE ]]; then
            recv_pipeline+="rbin.recv_rtp_sink_1 qd. ! rtpquicdemux rtcp-flow-id=$((${receiving_audio_flow_id}+1)) ! rbin.recv_rtcp_sink_1 rbin.send_rtcp_src_1 ! rtpquicmux rtcp-flow-id=$((${receiving_audio_flow_id}+1)) use-datagram=TRUE ! qm. rbin.recv_rtp_src_1_${receiving_ssrc}_${receiving_audio_pt} ! "
        fi
        recv_pipeline+="rtpopusdepay ! queue2 ! decodebin ! queue2 ! autoaudiosink "
    fi
fi

if [[ $participant_mode == "send" || $participant_mode == "bidi" ]]; then
    send_pipeline+="videotestsrc num-buffers=$((${video_framerate}*${video_secs})) is-live=true ! videoconvert ! video/x-raw,width=${video_x_res},height=${video_y_res},framerate=${video_framerate}/1 ! vp8enc ! rtpvp8pay mtu=${mtu} pt=${sending_video_pt} ssrc=${sending_ssrc} ! "
    if [[ $rtcp == TRUE ]]; then
        send_pipeline+="rbin.send_rtp_sink_2 qd. ! rtpquicdemux rtcp-flow-id=$((${sending_video_flow_id}+1)) ! rbin.recv_rtcp_sink_2 rbin.send_rtcp_src_2 ! rtpquicmux rtcp-flow-id=$((${sending_video_flow_id}+1)) use-datagram=TRUE ! qm. rbin.send_rtp_src_2 ! "
    fi
    send_pipeline+="rtpquicmux rtp-flow-id=${sending_video_flow_id} ${roq_mux_opts} ! qm. "

    if [[ $multi_stream == TRUE ]]; then
        send_pipeline+="audiotestsrc is-live=true ! audioconvert ! opusenc ! rtpopuspay mtu=${mtu} pt=${sending_audio_pt} ssrc=${sending_ssrc} ! "
        if [[ $rtcp == TRUE ]]; then
            send_pipeline+="rbin.send_rtp_sink_3 qd. ! rtpquicdemux rtcp-flow-id=$((${sending_audio_flow_id}+1)) ! rbin.recv_rtcp_sink_3 rbin.send_rtcp_src_3 ! rtpquicmux rtcp-flow-id=$((${sending_audio_flow_id}+1)) use-datagram=TRUE ! qm. rbin.send_rtp_src_3 ! "
        fi
        send_pipeline+="rtpquicmux rtp-flow-id=${sending_audio_flow_id} ${roq_mux_opts} ! qm. "
    fi
fi

check_gst_element "quicsrc"; if [[ $? -ne 0 ]]; then exit -1; fi
check_gst_element "quicsink"; if [[ $? -ne 0 ]]; then exit -1; fi
check_gst_element "quicdemux"; if [[ $? -ne 0 ]]; then exit -1; fi
check_gst_element "quicmux"; if [[ $? -ne 0 ]]; then exit -1; fi
check_gst_element "rtpquicdemux"; if [[ $? -ne 0 ]]; then exit -1; fi
check_gst_element "rtpquicmux"; if [[ $? -ne 0 ]]; then exit -1; fi

echo "### GStreamer RTP-over-QUIC Interop ###"


echo "Running GStreamer..."

set -x

gst-launch-1.0 -e ${rtpbin} ${recv_pipeline} ${send_pipeline}
#echo "GStreamer pipeline:"
#echo "gst-launch-1.0 -e ${rtpbin} ${recv_pipeline} ${send_pipeline}"
