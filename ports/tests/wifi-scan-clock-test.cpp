#include "../hardware/wifi-scan-clock.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
int main()
{
    uint64_t stamp=99;
    assert(AuroraScanObservedUs(5000000,37,&stamp) && stamp==4963000);
    assert(AuroraScanObservedUs(100000000,10000,&stamp) && stamp==90000000);
    // A genuinely old entry must stay older than a new scan's validity window.
    assert(stamp<99000000);
    assert(AuroraScanObservedUs(100000000,0,&stamp) && stamp==100000000);
    stamp=123;assert(!AuroraScanObservedUs(900,1,&stamp) && stamp==123);
    assert(!AuroraScanObservedUs(1000000,UINT32_MAX,&stamp));
    assert(AuroraScanObservedUs((uint64_t)UINT32_MAX*1000,UINT32_MAX,&stamp) && stamp==0);
    assert(!AuroraScanObservedUs(5000000,0,nullptr));
    puts("WIFI_SCAN_CLOCK_UNIT_PASS fresh/stale/units/age-overflow/no-output-on-error");
}
