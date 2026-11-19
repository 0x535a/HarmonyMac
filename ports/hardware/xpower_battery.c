/* SPDX-License-Identifier: GPL-3.0-only
 * Keep the ISO's audited XPower/Dubai interface and renamed imports. Only
 * replace platform-specific path selection; no global syscall interposition.
 * The adjacent compatibility implementation originated in the L410 port.
 */
#define _GNU_SOURCE
#include "power_supply.h"
#define L410_BATTERY_ROOT AuroraBatteryRoot()
#include "xpower_battery_v303.c"
