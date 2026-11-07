/* SPDX-License-Identifier: GPL-3.0-only */
#include "include/aurora_drm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

int AuroraOpenDisplayDrm(AuroraDrmDisplay *display, int own_display)
{
    if (!display) { errno = EINVAL; return -1; }
    AuroraDrmDisplay best = {.fd = -1};
    int best_score = -1;
    for (int index = 0; index < 16; ++index) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", index);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmVersion *version = drmGetVersion(fd);
        if (!version || !version->name || !strcmp(version->name, "simpledrm")) {
            if (version) drmFreeVersion(version);
            close(fd);
            continue;
        }
        drmModeRes *resources = drmModeGetResources(fd);
        if (!resources) { drmFreeVersion(version); close(fd); continue; }
        for (int n = 0; n < resources->count_connectors; ++n) {
            drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[n]);
            if (!connector) continue;
            if (connector->connection != DRM_MODE_CONNECTED || !connector->count_modes) {
                drmModeFreeConnector(connector);
                continue;
            }
            int score = !strcmp(version->name, "appledrm") ? 100 : 10;
            if (connector->connector_type == DRM_MODE_CONNECTOR_eDP ||
                connector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
                connector->connector_type == DRM_MODE_CONNECTOR_DSI) score += 100;
            if (score <= best_score) { drmModeFreeConnector(connector); continue; }
            uint32_t crtc = 0, crtc_index = 0;
            for (int e = 0; e < connector->count_encoders && !crtc; ++e) {
                drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoders[e]);
                if (!encoder) continue;
                for (int c = 0; c < resources->count_crtcs; ++c) {
                    if (encoder->possible_crtcs & (1U << c)) {
                        if (!crtc || resources->crtcs[c] == encoder->crtc_id) {
                            crtc = resources->crtcs[c];
                            crtc_index = c;
                        }
                    }
                }
                drmModeFreeEncoder(encoder);
            }
            if (!crtc) { drmModeFreeConnector(connector); continue; }
            int mode = 0;
            for (int m = 0; m < connector->count_modes; ++m)
                if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = m; break; }
            if (best.fd >= 0) close(best.fd);
            best.fd = dup(fd);
            if (best.fd < 0) { drmModeFreeConnector(connector); continue; }
            fcntl(best.fd, F_SETFD, FD_CLOEXEC);
            best.connector = connector->connector_id;
            best.crtc = crtc;
            best.crtc_index = crtc_index;
            best.mode = connector->modes[mode];
            best.mm_width = connector->mmWidth;
            best.mm_height = connector->mmHeight;
            snprintf(best.driver, sizeof(best.driver), "%s", version->name);
            best_score = score;
            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
        drmFreeVersion(version);
        close(fd);
    }
    if (best.fd < 0) { errno = ENODEV; return -1; }
    if (own_display) {
        if (drmSetMaster(best.fd)) { close(best.fd); return -1; }
    } else {
        /* A primary-node open can become master automatically. Allocator/EGL
         * clients must release only their own master before composer starts. */
        (void)drmDropMaster(best.fd);
    }
    *display = best;
    return 0;
}
