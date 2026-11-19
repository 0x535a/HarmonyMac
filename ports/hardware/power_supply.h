/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AURORA_POWER_SUPPLY_H
#define AURORA_POWER_SUPPLY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Enumerates Linux class entries by type=Battery, not by a platform name.
 * On absent, malformed, or ambiguous devices returns -1, setting errno.
 */
int AuroraFindBattery(char *path, size_t size);
const char *AuroraBatteryRoot(void);
#ifdef __cplusplus
}
#endif
#endif
