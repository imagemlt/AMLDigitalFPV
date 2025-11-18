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

void *aml_display_thread(void *unused)
{
  // Track whether we were skipping frames in the previous iteration
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
      fprintf(stderr, "VIDIOC_DQBUF failed: %d\n", errno);
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
        fprintf(stderr, "VIDIOC_DQBUF (drain) failed: %d\n", errno);
        if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
        {
          fprintf(stderr, "VIDIOC_QBUF failed while recovering: %d\n", errno);
        }
        return NULL;
      }
      // Earlier frame becomes obsolete, requeue immediately.
      {
        uint64_t now_ms = get_time_ms();
        fprintf(stdout,
                "[%llu ms] display_thread: backlog detected, dropping idx=%u\n",
                (unsigned long long)now_ms, latest.index);
        fflush(stdout);
      }
      if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
      {
        fprintf(stderr, "VIDIOC_QBUF failed while draining: %d\n", errno);
        ioctl(videoFd, VIDIOC_QBUF, &candidate);
        return NULL;
      }

      latest = candidate;
      drained++;
    }

    if (drained > 0)
    {
      backlog_active = true;
      fprintf(stdout, "[%llu ms] display_thread: skipped %d stale frame(s), displaying idx=%u\n",
              (unsigned long long)get_time_ms(), drained, latest.index);
      fflush(stdout);
    }
    else
    {
      backlog_active = false;
    }

    /*
     fprintf(stdout, "[%llu ms] display_thread: skipped %d stale frame(s), displaying idx=%u, timestamp=%ld.%06ld Timecode: %02d:%02d:%02d:%02d memory %d\n",
             (unsigned long long)get_time_ms(), drained, latest.index, latest.timestamp.tv_sec, latest.timestamp.tv_usec, latest.timecode.hours, latest.timecode.minutes, latest.timecode.seconds, latest.timecode.frames, latest.memory);
     fflush(stdout);
     */
    if (ioctl(videoFd, VIDIOC_QBUF, &latest) < 0)
    {
      fprintf(stderr, "VIDIOC_QBUF failed: %d\n", errno);
      break;
    }
    count++;
  }
  printf("Display thread terminated\n");
  return NULL;
}

int aml_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags)
{
  codecParam.handle = -1;
  codecParam.cntl_handle = -1;
  codecParam.audio_utils_handle = -1;
  codecParam.sub_handle = -1;
  codecParam.has_video = 1;
  codecParam.noblock = 1;
  codecParam.stream_type = STREAM_TYPE_ES_VIDEO;
  codecParam.am_sysinfo.param = 0;

  system("echo 0 > /sys/class/video/disable_video");
  system("echo 1 >/sys/class/graphics/fb0/blank");

  // #ifdef STREAM_TYPE_FRAME
  codecParam.dec_mode = STREAM_TYPE_FRAME;
  printf("Using FRAME mode\n");
  // #endif

  // #ifdef FRAME_BASE_PATH_AMLVIDEO_AMVIDEO
  printf("Using video path: %d\n", FRAME_BASE_PATH_AMLVIDEO_AMVIDEO);
  codecParam.video_path = FRAME_BASE_PATH_AMLVIDEO_AMVIDEO;

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
    printf("hevc special low latency flag enabled!!!\n");
    printf(low_latency_cfg);
    printf(" \n===\n");
    codecParam.config = strdup(low_latency_cfg);
    if (codecParam.config)
    {
      codecParam.config_len = strlen(low_latency_cfg);
    }
    else
    {
      fprintf(stderr, "Failed to allocate low latency config string\n");
    }
    printf("CaoCaoCao!!!!\n");
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
    printf("Video format not supported\n");
    return -1;
  }
  printf("try codec init !!!!!\n");

  codecParam.am_sysinfo.width = width;
  codecParam.am_sysinfo.height = height;
  codecParam.am_sysinfo.rate = 90000 / redrawRate;
  codecParam.am_sysinfo.param = (void *)((size_t)codecParam.am_sysinfo.param | SYNC_OUTSIDE);

  codecParam.vbuf_size = 1920 * 1080 * 1;
  int ret;
  printf("try codec init !!!!!\n");
  if ((ret = codec_init(&codecParam)) != 0)
  {
    fprintf(stderr, "codec_init error: %d\n", ret);
    return -2;
  }
  printf("try codec init !!!!!\n");

  if ((ret = codec_set_freerun_mode(&codecParam, 1)) != 0)
  {
    fprintf(stderr, "Can't set Freerun mode: %x\n", ret);
    return -2;
  }

  if ((ret = codec_set_syncenable(&codecParam, 0)) != 0)
  {
    fprintf(stderr, "Can't set syncenable: %x\n", ret);
    return -2;
  }

  if ((ret = codec_set_video_delay_limited_ms(&codecParam, 0)) != 0)
  {
    fprintf(stderr, "Can't set video delay limited ms: %x\n", ret);
    return -2;
  }

  if ((ret = codec_disalbe_slowsync(&codecParam, 1)) != 0)
  {
    fprintf(stderr, "Can't set disable slowsync: %x\n", ret);
    return -2;
  }

  if ((ret = codec_set_cntl_avthresh(&codecParam, 0)) != 0)
  {
    fprintf(stderr, "Can't set cntl mode: %x\n", ret);
    return -2;
  }

  printf("prepare VFM MAP\n");
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
    printf("VFM map: %s\n", vfm_map);
    // if (strstr(vfm_map, "amlvideo"))
    //{
    printf("Using display thread for amlvideo pipeline\n");

    videoFd = open("/dev/video10", O_RDONLY | O_NONBLOCK);
    if (videoFd < 0)
    {
      fprintf(stderr, "Failed to open video device: %d\n", errno);
      return -3;
    }

    pthread_create(&displayThread, NULL, aml_display_thread, NULL);
    //}
  }
  else
  {
    printf("Failed to read VFM map\n");
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

  codec_checkin_pts(&codecParam, get_time_ms());

  api = codec_write(&codecParam, decodeUnit, length);
  if (api < 0)
  {
    if (errno != EAGAIN)
    {
      fprintf(stderr, "codec_write() error: %x %d\n", errno, api);
      codec_reset(&codecParam);
    }
    else
    {
      if (++errCounter == MAX_WRITE_ATTEMPTS)
      {
        fprintf(stderr, "codec_write() timeout\n");
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
