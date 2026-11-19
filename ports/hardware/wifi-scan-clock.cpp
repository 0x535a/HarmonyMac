/* SPDX-License-Identifier: Apache-2.0
 * Adapt the stock vendor HAL scan timestamps to the stock framework's boot
 * clock. All radio, scan, callback, power and interface operations remain in
 * the byte-identical original HAL. No scan entry or age is manufactured.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "wifi-scan-clock.h"
#include "v2_0/chip_types.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <time.h>
using ScanInfo=OHOS::HDI::Wlan::Chip::V2_0::ScanResultsInfo;
using ScanFn=int(*)(void *,std::vector<ScanInfo>&);
using InitFn=int(*)(void *);
static_assert(sizeof(void *)==8 && sizeof(ScanFn)==8);
static_assert(sizeof(ScanInfo)==136 && offsetof(ScanInfo,age)==52 && offsetof(ScanInfo,tsf)==56);
// Verified against both the public2.0 table and the pinned product initializer.
constexpr size_t SCAN_SLOT_OFFSET=176;
static std::atomic<ScanFn> original_scan{nullptr};
static std::atomic<unsigned> log_count{0};
static bool boot_us(uint64_t &value)
{
    timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t) || t.tv_sec<0 || t.tv_nsec<0)return false;
    value=(uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;return true;
}
static int get_scan(void *iface,std::vector<ScanInfo>& results)
{
    ScanFn original=original_scan.load(std::memory_order_acquire);uint64_t start;
    if(!original || !boot_us(start))return -1;
    int rc=original(iface,results);if(rc)return rc;
    if(results.size()>1024)return -5;
    for(const auto &entry:results) {
        uint64_t observed;
        if(entry.bssid.size()!=6 || entry.ie.size()>65535 || entry.beaconIe.size()>65535 ||
           !AuroraScanObservedUs(start,entry.age,&observed)) {
            std::fprintf(stderr,"AURORA_WIFI_SCAN_CLOCK_REJECT invalid ABI/age count=%zu\n",results.size());
            return -5;
        }
    }
    uint64_t first_tsf=results.empty()?0:results.front().tsf;
    uint32_t first_age=results.empty()?0:results.front().age;
    for(auto &entry:results) {
        uint64_t observed;
        if(!AuroraScanObservedUs(start,entry.age,&observed))return -5;
        entry.tsf=observed;
    }
    if(log_count.fetch_add(1)<16)
        std::fprintf(stderr,"AURORA_WIFI_SCAN_CLOCK count=%zu query_boot_us=%llu first_ap_tsf_us=%llu first_age_ms=%u first_observed_us=%llu\n",
            results.size(),(unsigned long long)start,(unsigned long long)first_tsf,first_age,
            (unsigned long long)(results.empty()?0:results.front().tsf));
    return rc;
}
extern "C" __attribute__((visibility("default"))) int InitWifiVendorHalFuncTable(void *table)
{
    if(!table)return -1;
    static std::mutex lock;std::lock_guard<std::mutex> guard(lock);
    static void *legacy=nullptr;static InitFn original_init=nullptr;
    if(!legacy) {
        legacy=dlopen("/vendor/lib64/libwifi_hal_hw.aurora-original.z.so",RTLD_NOW|RTLD_LOCAL);
        if(!legacy){std::fprintf(stderr,"AURORA_WIFI_SCAN_CLOCK_DLOPEN_FAIL %s\n",dlerror());return -1;}
        original_init=reinterpret_cast<InitFn>(dlsym(legacy,"InitWifiVendorHalFuncTable"));
    }
    Dl_info init_info{};
    if(!original_init || original_init==&InitWifiVendorHalFuncTable ||
       !dladdr(reinterpret_cast<void *>(original_init),&init_info) ||
       reinterpret_cast<uintptr_t>(original_init)-reinterpret_cast<uintptr_t>(init_info.dli_fbase)!=0x658c)
        return -5;
    int rc=original_init(table);if(rc)return rc;
    ScanFn candidate;std::memcpy(&candidate,static_cast<char *>(table)+SCAN_SLOT_OFFSET,sizeof(candidate));
    Dl_info info{};
    if(!candidate || !dladdr(reinterpret_cast<void *>(candidate),&info) ||
       info.dli_fbase!=init_info.dli_fbase ||
       reinterpret_cast<uintptr_t>(candidate)-reinterpret_cast<uintptr_t>(info.dli_fbase)!=0x10f9c)
        return -5;
    ScanFn prior=original_scan.load();if(prior && prior!=candidate)return -5;
    original_scan.store(candidate,std::memory_order_release);
    ScanFn replacement=&get_scan;std::memcpy(static_cast<char *>(table)+SCAN_SLOT_OFFSET,&replacement,sizeof(replacement));
    return rc;
}
