/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int AuroraGetKeyboardBacklight(uint32_t *level);
int AuroraSetKeyboardBacklight(uint32_t level);
#ifdef __cplusplus
}
#endif
