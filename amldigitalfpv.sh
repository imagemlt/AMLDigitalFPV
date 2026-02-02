#!/bin/bash


if [ -f /storage/digitalfpv/wfb.conf ]; then
	. /storage/digitalfpv/wfb.conf
else
	. /flash/wfb.conf
fi

if [ -f /storage/streamer/AMLDigitalFPV.new ]; then
	mv /storage/streamer/AMLDigitalFPV.new /storage/streamer/AMLDigitalFPV
fi

pactl load-module module-alsa-sink device=hdmi:CARD=AMLAUGESOUND,DEV=0 sink_name=hdmi_out || true
pactl set-default-sink hdmi_out || true
pactl set-sink-volume @DEFAULT_SINK@ ${sound_volume}% || true

export GST_PLUGIN_PATH=/storage/streamer/gstreamer/
export GST_PLUGIN_SCANNER=/storage/streamer/gstreamer/gst-plugin-scanner
export LD_LIBRARY_PATH=$GST_PLUGIN_PATH

if [ ! -f /tmp/added_vfm ] ; then
	echo '[+]add VFM map'
	echo 'add vdec-map-0 vdec.h265.00 amvideo' >/sys/class/vfm/map
	touch /tmp/added_vfm
fi

if [ "$bad_frame" != "0" ]; then
	echo "[+]Error policy: Ignore ERROR"
	echo 1 > /sys/module/amvdec_h265/parameters/hacked_lowlatency
	#echo 0 >/sys/module/amvdec_h265/parameters/nal_skip_policy
	echo 0 >/sys/module/amvdec_h265/parameters/ref_frame_mark_flag
	echo ${bad_frame} > /sys/module/amvdec_h265/parameters/error_handle_policy
else
	echo "[+]Error policy: Drop FRAME"
fi

/storage/streamer/AMLDigitalFPV -w ${video_width} -h ${video_height} -p 120 -a ${sound} -l ${buf_level}
