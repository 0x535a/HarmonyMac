/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AURORA_BACKLIGHT_H
#define AURORA_BACKLIGHT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 0..255 logical brightness. Returns -1 with errno; no synthetic success. */
int AuroraGetBacklight(uint32_t *level);
int AuroraSetBacklight(uint32_t level);
#ifdef __cplusplus
}
#endif
#endif
