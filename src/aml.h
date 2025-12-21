#ifdef __cplusplus
extern "C"
{
#endif

#include <sys/utsname.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <codec.h>
#include <errno.h>
#include <pthread.h>

#include <linux/videodev2.h>

#include "util.h"
#include "spdlog_wrapper.h"

#define SYNC_OUTSIDE 0x02
#define UCODE_IP_ONLY_PARAM 0x08
#define MAX_WRITE_ATTEMPTS 5
#define EAGAIN_SLEEP_TIME 2 * 1000

#define DISPLAY_FULLSCREEN 1
#define ENABLE_HARDWARE_ACCELERATION_1 2
#define ENABLE_HARDWARE_ACCELERATION_2 4
#define DISPLAY_ROTATE_MASK 24
#define DISPLAY_ROTATE_90 8
#define DISPLAY_ROTATE_180 16
#define DISPLAY_ROTATE_270 24

#define INIT_EGL 1
#define INIT_VDPAU 2
#define INIT_VAAPI 3

#define INITIAL_DECODER_BUFFER_SIZE (256 * 1024)

#define VIDEO_FORMAT_MASK_H265 0
#define VIDEO_FORMAT_MASK_H264 1
#define VIDEO_FORMAT_MASK_AV1 2

    void *aml_display_thread(void *unused);
    int aml_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags, int framePath, int streamType);
    void aml_cleanup();
    int aml_submit_decode_unit(uint8_t *decodeUnit, size_t size);

#ifdef __cplusplus
}
#endif