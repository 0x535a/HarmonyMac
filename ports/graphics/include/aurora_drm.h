/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AURORA_DRM_H
#define AURORA_DRM_H
#include <stdint.h>
#include <xf86drmMode.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int fd;
    uint32_t connector;
    uint32_t crtc;
    uint32_t crtc_index;
    uint32_t mm_width;
    uint32_t mm_height;
    drmModeModeInfo mode;
    char driver[64];
} AuroraDrmDisplay;
int AuroraOpenDisplayDrm(AuroraDrmDisplay *display, int own_display);
#ifdef __cplusplus
}
#endif
#endif
