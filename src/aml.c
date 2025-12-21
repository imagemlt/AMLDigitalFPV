/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 * Copyright (C) 2016 OtherCrashOverride, Daniel Mehrwald
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "aml.h"

static codec_para_t codecParam = {0};
static pthread_t displayThread;
static int videoFd = -1;
static volatile bool done = false;
void *pkt_buf = NULL;
size_t pkt_buf_size = 0;
static uint64_t last_pts_us = 0;

void *aml_display_thread(void *unused)
{
  // Track whether we were skipping frames in the previous iteration
  set_priority("DisplayThread", 35);
  bool backlog_active = false;
  int count = 0;
  while (!done)
  {
    struct v4l2_buffer latest = {0};
    latest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(videoFd, VIDIOC_DQBUF, &latest) < 0)
    {
      if (errno == EAGAIN)
      {
        usleep(500);
        continue;
      }
      spdlog_error("VIDIOC_DQBUF failed: %d", errno);
      break;
    }

    int drained = 0;
    while (!done)
    {
      struct v4l2_buffer candidate = {0};
      candidate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(videoFd, VIDIOC_DQBUF, &candidate) < 0)
      {
        if (errno == EAGAIN)
        {
          backlog_active = false;
          break;
        }
        spdlog_error("VIDIOC_DQBUF (drain) failed: %d", errno);
        if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
        {
          spdlog_error("VIDIOC_QBUF failed while recovering: %d", errno);
        }
        return NULL;
      }
      // Earlier frame becomes obsolete, requeue immediately.
      {
        uint64_t now_ms = get_time_ms();
        spdlog_info(
            "[%llu ms] display_thread: backlog detected, dropping idx=%u",
            (unsigned long long)now_ms, latest.index);
      }
      if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
      {
        spdlog_error("VIDIOC_QBUF failed while draining: %d", errno);
        ioctl(videoFd, VIDIOC_QBUF, &candidate);
        return NULL;
      }

      latest = candidate;
      drained++;
    }

    if (drained > 0)
    {
      backlog_active = true;
      spdlog_info("[%llu ms] display_thread: skipped %d stale frame(s), displaying idx=%u",
                  (unsigned long long)get_time_ms(), drained, latest.index);
    }
    else
    {
      backlog_active = false;
    }

    if (count % 60 == 0)
    {
      spdlog_info("[%llu ms] display_thread: skipped %d stale frame(s), displaying idx=%u, timestamp=%ld.%06ld Timecode: %02d:%02d:%02d:%02d memory %d",
                  (unsigned long long)get_time_ms(), drained, latest.index, latest.timestamp.tv_sec, latest.timestamp.tv_usec, latest.timecode.hours, latest.timecode.minutes, latest.timecode.seconds, latest.timecode.frames, latest.memory);
    }
    if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
    {
      spdlog_error("VIDIOC_QBUF failed: %d", errno);
      break;
    }
    count++;
  }
  spdlog_info("Display thread terminated");
  return NULL;
}

int aml_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags, int framePath, int streamType)
{
  codecParam.handle = -1;
  codecParam.cntl_handle = -1;
  codecParam.audio_utils_handle = -1;
  codecParam.sub_handle = -1;
  codecParam.has_video = 1;
  codecParam.noblock = 1;
  if (streamType == 0)
  {
    codecParam.stream_type = STREAM_TYPE_ES_VIDEO;
    spdlog_info("Using ES_VIDEO mode");
  }
  else
  {
    codecParam.stream_type = STREAM_TYPE_FRAME;
    spdlog_info("USING FRAME mode");
  }
  codecParam.am_sysinfo.param = 0;

  // system("echo 0 > /sys/class/video/disable_video");
  write_sysfs("/sys/class/video/disable_video", "0\n");
  // system("cat /dev/zero >/dev/fb0");
  // write_sysfs("/sys/class/graphics/fb0/blank", "0\n");

  // minimal vfm map
  // system("echo 'add vdec-map-0 vdec.h265.00 amvideo' >/sys/class/vfm/map");
  // #ifdef STREAM_TYPE_FRAME
  codecParam.dec_mode = STREAM_TYPE_FRAME;
  spdlog_info("Using FRAME mode");
  // #endif

  // #ifdef FRAME_BASE_PATH_AMLVIDEO_AMVIDEO
  if (framePath != 1)
  {
    spdlog_info("Using video path: %d", FRAME_BASE_PATH_AMLVIDEO_AMVIDEO);
    codecParam.video_path = FRAME_BASE_PATH_AMLVIDEO_AMVIDEO;
  }
  else
  {
    spdlog_info("Using video path: %d", FRAME_BASE_PATH_AMVIDEO);
    // codecParam.video_path = FRAME_BASE_PATH_AMLVIDEO_AMVIDEO;
    codecParam.video_path = FRAME_BASE_PATH_AMVIDEO;
  }
  // #endif

  if (videoFormat == VIDEO_FORMAT_MASK_H264)
  {
    if (width > 1920 || height > 1080)
    {
      codecParam.video_type = VFORMAT_H264_4K2K;
      codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264_4K2K;
    }
    else
    {
      codecParam.video_type = VFORMAT_H264;
      codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264;

      // Workaround for decoding special case of C1, 1080p, H264
      int major, minor;
      struct utsname name;
      uname(&name);
      int ret = sscanf(name.release, "%d.%d", &major, &minor);
      if (ret == 2 && !(major > 3 || (major == 3 && minor >= 14)) && width == 1920 && height == 1080)
        codecParam.am_sysinfo.param = (void *)UCODE_IP_ONLY_PARAM;
    }
  }
  else if (videoFormat == VIDEO_FORMAT_MASK_H265)
  {
    codecParam.video_type = VFORMAT_HEVC;
    codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
    const char *low_latency_cfg =
        "parm_v4l_low_latency_mode:1;"
        "parm_v4l_buffer_margin:1;"
        "parm_enable_fence:0;"
        "parm_fence_usage:0;"
        "hevc_double_write_mode:0;";
    if (codecParam.config)
    {
      free(codecParam.config);
      codecParam.config = NULL;
      codecParam.config_len = 0;
    }
    spdlog_info("hevc special low latency flag enabled: %s", low_latency_cfg);
    // printf(low_latency_cfg);

    codecParam.config = strdup(low_latency_cfg);
    if (codecParam.config)
    {
      codecParam.config_len = strlen(low_latency_cfg);
    }
    else
    {
      spdlog_error("Failed to allocate low latency config string");
    }
#ifdef CODEC_TAG_AV1
  }
  else if (videoFormat == VIDEO_FORMAT_MASK_AV1)
  {
    codecParam.video_type = VFORMAT_AV1;
    codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_AV1;
#endif
  }
  else
  {
    spdlog_error("Video format not supported");
    return -1;
  }

  codecParam.am_sysinfo.width = width;
  codecParam.am_sysinfo.height = height;
  codecParam.am_sysinfo.rate = 90000 / redrawRate;
  codecParam.am_sysinfo.param = (void *)((size_t)codecParam.am_sysinfo.param | SYNC_OUTSIDE);

  codecParam.vbuf_size = width * height * 1;
  int ret;

  if ((ret = codec_init(&codecParam)) != 0)
  {
    spdlog_error("codec_init error: %x", ret);
    return -2;
  }

  if ((ret = codec_set_freerun_mode(&codecParam, 1)) != 0)
  {
    spdlog_error("Can't set Freerun mode: %x", ret);
    return -2;
  }

  if ((ret = codec_set_syncenable(&codecParam, 0)) != 0)
  {
    spdlog_error("Can't set syncenable: %x", ret);
    return -2;
  }

  if ((ret = codec_set_video_delay_limited_ms(&codecParam, 0)) != 0)
  {
    spdlog_error("Can't set video delay limited ms: %x", ret);
    return -2;
  }

  if ((ret = codec_disalbe_slowsync(&codecParam, 1)) != 0)
  {
    spdlog_error("Can't set disable slowsync: %x\n", ret);
    return -2;
  }

  if ((ret = codec_set_cntl_avthresh(&codecParam, 0)) != 0)
  {
    spdlog_error("Can't set cntl mode: %x\n", ret);
    return -2;
  }

  // sysfs tuning
  write_sysfs("/sys/class/video/video_delay_val", "0\n");
  write_sysfs("/sys/class/video/vsync_pts_inc_upint", "2000\n"); // 可按需要再调
  write_sysfs("/sys/class/video/show_first_frame_nosync", "1\n");
  write_sysfs("/sys/class/video/slowsync_repeat_enable", "0\n");
  write_sysfs("/sys/class/video/vframe_walk_delay", "0\n");
  write_sysfs("/sys/class/video/last_required_total_delay", "0\n");
  /* 如果内核带自定义补丁 */
  write_sysfs("/sys/class/video/hack_novsync", "1\n");
  write_sysfs("/sys/class/video/hold_video", "0\n");
  write_sysfs("/sys/class/video/freerun_mode", "1\n"); // FREERUN_NODUR
  write_sysfs("/sys/class/video/show_first_picture", "1\n");
  write_sysfs("/sys/class/video/enable_hdmi_delay_normal_check", "0\n");
  write_sysfs("/sys/class/video/hdmin_delay_start", "0\n");
  write_sysfs("/sys/class/video/hdmin_delay_max_ms", "0\n");
  write_sysfs("/sys/class/video/free_keep_buffer", "1\n");
  /* 降低缩放负载（画质下降） */
  write_sysfs("/sys/class/video/hscaler_8tap_en", "0\n");
  write_sysfs("/sys/class/video/pre_hscaler_ntap_en", "0\n");
  write_sysfs("/sys/class/video/pre_vscaler_ntap_en", "0\n");
  write_sysfs("/sys/class/video/pip_pre_hscaler_ntap_en", "0\n");

  /* HDMI /sys/class/amhdmitx/amhdmitx0/ */
  write_sysfs("/sys/class/amhdmitx/amhdmitx0/attr", "rgb\n"); // 或 "rgb"
  write_sysfs("/sys/class/amhdmitx/amhdmitx0/frac_rate_policy", "0\n");
  write_sysfs("/sys/class/amhdmitx/amhdmitx0/allm_mode", "1\n");
  write_sysfs("/sys/class/amhdmitx/amhdmitx0/contenttype_mode", "1\n");

  spdlog_info("prepare VFM MAP");
  // if ((ret = codec_set_av_threshold(&codecParam, 0)) != 0)
  //{
  //   fprintf(stderr, "Can't set av thresholdc: %x\n", ret);
  //   return -2;
  // }
  char vfm_map[2048] = {};
  char *eol;
  if (read_file("/sys/class/vfm/map", vfm_map, sizeof(vfm_map) - 1) > 0 && (eol = strchr(vfm_map, '\n')))
  {
    *eol = 0;

    // If amlvideo is in the pipeline, we must spawn a display thread
    spdlog_info("VFM map: %s", vfm_map);
    // if (strstr(vfm_map, "amlvideo"))
    //{
    spdlog_info("Using display thread for amlvideo pipeline");

    videoFd = open("/dev/video10", O_RDONLY | O_NONBLOCK);
    if (videoFd < 0)
    {
      spdlog_error("Failed to open video device: %d", errno);
      return -3;
    }

    pthread_create(&displayThread, NULL, aml_display_thread, NULL);
    //}
  }
  else
  {
    spdlog_error("Failed to read VFM map");
  }

  ensure_buf_size(&pkt_buf, &pkt_buf_size, INITIAL_DECODER_BUFFER_SIZE);

  return 0;
}

void aml_cleanup()
{
  if (codecParam.config)
  {
    free(codecParam.config);
    codecParam.config = NULL;
    codecParam.config_len = 0;
  }
  if (videoFd >= 0)
  {
    done = true;
    pthread_join(displayThread, NULL);
    close(videoFd);
  }

  codec_close(&codecParam);
  free(pkt_buf);
}

int aml_submit_decode_unit(uint8_t *decodeUnit, size_t length)
{

  // ensure_buf_size(&pkt_buf, &pkt_buf_size, decodeUnit->fullLength);

  int written = 0, errCounter = 0, api;

  // codec_checkin_pts(&codecParam, get_time_ms() - 8);

  uint64_t pts_us = (get_time_ms() - 8) * 1000ULL;
  if (pts_us <= last_pts_us)
    pts_us = last_pts_us + 1000; // 至少 +1ms
  last_pts_us = pts_us;

  codec_checkin_pts_us64(&codecParam, pts_us);

  api = codec_write(&codecParam, decodeUnit, length);
  if (api < 0)
  {
    if (errno != EAGAIN)
    {
      spdlog_error("codec_write() error: %x %d", errno, api);
      codec_reset(&codecParam);
    }
    else
    {
      if (++errCounter == MAX_WRITE_ATTEMPTS)
      {
        spdlog_error("codec_write() timeout");
      }
      usleep(EAGAIN_SLEEP_TIME);
    }
  }
  else
  {
  }

  return api;
}

void measure_latency_breakdown()
{
  struct timespec ts;
  static struct timespec last_input, last_decode, last_display;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  // 测量各阶段延迟
  struct buf_status vbuf;
  codec_get_vbuf_state(&codecParam, &vbuf);

  int current_delay;
  codec_get_video_cur_delay_ms(&codecParam, &current_delay);

  struct vdec_status vstatus;
  codec_get_vdec_state(&codecParam, &vstatus);

  printf("=== Latency Breakdown ===\n");
  printf("Buffer: %d/%d bytes\n", vbuf.data_len, vbuf.size);
  printf("Current Delay: %dms\n", current_delay);
  printf("Decoder Status: 0x%x\n", vstatus.status);
  printf("Frame Rate: %d fps\n", vstatus.fps);
  printf("=======================\n");
}
