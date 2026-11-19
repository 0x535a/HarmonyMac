/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
// Linux cfg80211 age is local elapsed milliseconds, not the AP's TSF clock.
// Using the pre-query boot clock is conservative about query transit latency.
static inline bool AuroraScanObservedUs(uint64_t query_boot_us,uint32_t age_ms,uint64_t *observed_us)
{
    uint64_t elapsed=(uint64_t)age_ms*1000;
    if(!observed_us || elapsed>query_boot_us)return false;
    *observed_us=query_boot_us-elapsed;
    return true;
}
