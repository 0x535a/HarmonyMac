/* SPDX-License-Identifier: GPL-3.0-only */
#include "include/aurora_drm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static int internal_connector(uint32_t type)
{
    return type == DRM_MODE_CONNECTOR_eDP || type == DRM_MODE_CONNECTOR_LVDS ||
           type == DRM_MODE_CONNECTOR_DSI;
}

static void describe_connector(AuroraDrmDisplay *display, const drmModeConnector *connector)
{
    int preferred = 0;
    for (int i = 0; i < connector->count_modes; ++i) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            preferred = i;
            break;
        }
    }
    display->connector = connector->connector_id;
    display->connector_type = connector->connector_type;
    display->connector_type_id = connector->connector_type_id;
    display->mode = connector->modes[preferred];
    display->mm_width = connector->mmWidth;
    display->mm_height = connector->mmHeight;
    if (internal_connector(connector->connector_type) && !strcmp(display->driver, "appledrm")) {
        for (int i = 0; i < connector->count_modes; ++i) {
            const drmModeModeInfo *mode = &connector->modes[i];
            if (mode->hdisplay == display->mode.hdisplay && mode->vdisplay == display->mode.vdisplay &&
                mode->vrefresh > display->mode.vrefresh)
                display->mode = *mode;
        }
    }
}

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
            if (internal_connector(connector->connector_type)) score += 100;
            if (score <= best_score) { drmModeFreeConnector(connector); continue; }
            uint32_t crtc = 0, crtc_index = 0;
            for (int e = 0; e < connector->count_encoders && !crtc; ++e) {
                drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoders[e]);
                if (!encoder) continue;
                for (int c = 0; c < resources->count_crtcs && c < 32; ++c) {
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
            int selected_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            if (selected_fd < 0) { drmModeFreeConnector(connector); continue; }
            if (best.fd >= 0) close(best.fd);
            best.fd = selected_fd;
            best.crtc = crtc;
            best.crtc_index = crtc_index;
            snprintf(best.driver, sizeof(best.driver), "%s", version->name);
            describe_connector(&best, connector);
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

struct Route {
    AuroraDrmDisplay display;
    uint32_t possible;
};

int AuroraFindAdditionalDisplays(const AuroraDrmDisplay *primary, AuroraDrmDisplay outputs[2])
{
    drmModeRes *resources = drmModeGetResources(primary->fd);
    if (!resources) return -1;
    if (resources->count_connectors < 2 || resources->count_crtcs < 2) {
        drmModeFreeResources(resources);
        return 0;
    }
    struct Route *routes = calloc((size_t)resources->count_connectors, sizeof(*routes));
    if (!routes) {
        drmModeFreeResources(resources);
        return -1;
    }
    int count = 0, found = 0;
    const unsigned crtcs = resources->count_crtcs < 32 ? (unsigned)resources->count_crtcs : 32;
    const uint32_t mask = (UINT32_MAX >> (32 - crtcs)) & ~(UINT32_C(1) << primary->crtc_index);
    for (int i = 0; i < resources->count_connectors; ++i) {
        if (resources->connectors[i] == primary->connector) continue;
        drmModeConnector *connector = drmModeGetConnector(primary->fd, resources->connectors[i]);
        if (!connector) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes) {
            uint32_t possible = 0;
            for (int e = 0; e < connector->count_encoders; ++e) {
                drmModeEncoder *encoder = drmModeGetEncoder(primary->fd, connector->encoders[e]);
                if (!encoder) continue;
                possible |= encoder->possible_crtcs;
                drmModeFreeEncoder(encoder);
            }
            if (possible & mask) {
                routes[count].display = *primary;
                describe_connector(&routes[count].display, connector);
                routes[count++].possible = possible & mask;
            }
        }
        drmModeFreeConnector(connector);
    }

    // As in Mutter, each connected output needs a distinct compatible CRTC.
    // Try both external routes before accepting a single route, so that a
    // flexible encoder cannot consume the only CRTC available to another.
    for (int i = 0; i < count && found < 2; ++i) {
        if (!found) {
            outputs[0] = routes[i].display;
            outputs[0].crtc_index = (uint32_t)__builtin_ctz(routes[i].possible);
            found = 1;
        }
        for (uint32_t options = routes[i].possible; options && found < 2; options &= options - 1) {
            uint32_t crtc = (uint32_t)__builtin_ctz(options);
            for (int j = i + 1; j < count; ++j) {
                uint32_t other = routes[j].possible & ~(UINT32_C(1) << crtc);
                if (!other) continue;
                outputs[0] = routes[i].display;
                outputs[0].crtc_index = crtc;
                outputs[1] = routes[j].display;
                outputs[1].crtc_index = (uint32_t)__builtin_ctz(other);
                found = 2;
                break;
            }
        }
    }
    for (int i = 0; i < found; ++i) outputs[i].crtc = resources->crtcs[outputs[i].crtc_index];
    free(routes);
    drmModeFreeResources(resources);
    return found;
}

static int power_property(const AuroraDrmDisplay *display, uint32_t *id, uint64_t *value)
{
    if (!display || display->fd < 0 || !id || !value) { errno = EINVAL; return -1; }
    drmModeConnector *connector = drmModeGetConnector(display->fd, display->connector);
    if (!connector) return -1;
    int found = 0;
    for (int i = 0; i < connector->count_props; ++i) {
        drmModePropertyRes *property = drmModeGetProperty(display->fd, connector->props[i]);
        if (!property) continue;
        if (!strcmp(property->name, "DPMS") && (property->flags & DRM_MODE_PROP_ENUM)) {
            *id = property->prop_id;
            *value = connector->prop_values[i];
            found = 1;
        }
        drmModeFreeProperty(property);
        if (found) break;
    }
    drmModeFreeConnector(connector);
    if (!found) { errno = EOPNOTSUPP; return -1; }
    return 0;
}

int AuroraGetDisplayPower(const AuroraDrmDisplay *display, uint32_t *power)
{
    if (!power) { errno = EINVAL; return -1; }
    uint32_t property = 0;
    uint64_t value = 0;
    if (power_property(display, &property, &value)) return -1;
    if (value > DRM_MODE_DPMS_OFF) { errno = ERANGE; return -1; }
    *power = (uint32_t)value;
    return 0;
}

int AuroraSetDisplayPower(const AuroraDrmDisplay *display, uint32_t power)
{
    if (power > DRM_MODE_DPMS_OFF) { errno = EINVAL; return -1; }
    uint32_t property = 0;
    uint64_t previous = 0;
    if (power_property(display, &property, &previous)) return -1;
    return drmModeConnectorSetProperty(display->fd, display->connector, property, power);
}
