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
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t mm_width;
    uint32_t mm_height;
    drmModeModeInfo mode;
    char driver[64];
} AuroraDrmDisplay;
int AuroraOpenDisplayDrm(AuroraDrmDisplay *display, int own_display);
// Find up to two additional connected outputs on the primary's KMS card.
// The returned descriptions borrow the primary's fd.
int AuroraFindAdditionalDisplays(const AuroraDrmDisplay *primary, AuroraDrmDisplay outputs[2]);
int AuroraGetDisplayPower(const AuroraDrmDisplay *display, uint32_t *power);
int AuroraSetDisplayPower(const AuroraDrmDisplay *display, uint32_t power);
#ifdef __cplusplus
}
#endif
#endif
