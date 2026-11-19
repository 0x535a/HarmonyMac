/* SPDX-License-Identifier: Apache-2.0
 * Ordinary client of actual Thermal HDI 1.1. No raw device access or injection.
 */
#include "v1_1/ithermal_interface.h"
#include "v1_1/thermal_callback_stub.h"
#include <object_collector.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
using namespace OHOS;
using namespace OHOS::HDI::Thermal::V1_1;
static unsigned fds()
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    unsigned count = 0;
    while (auto *e = readdir(d)) if (e->d_name[0] != '.') ++count;
    closedir(d);
    return count;
}
class Observer final : public IThermalCallback {
public:
    std::atomic<unsigned> events{0};
    int32_t OnThermalDataEvent(const HdfThermalCallbackInfo &event) override
    {
        unsigned sequence = events.fetch_add(1) + 1;
        printf("THERMAL_CALLBACK sequence=%u count=%zu\n", sequence, event.info.size());
        for (const auto &sensor : event.info) printf("THERMAL_SAMPLE sequence=%u type=%s milli_celsius=%d\n",
            sequence, sensor.type.c_str(), sensor.temp);
        return 0;
    }
};
int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);
    alarm(45);
    bool absent = argc == 2 && !strcmp(argv[1], "--absent");
    if (argc > 2 || (argc == 2 && !absent)) return 2;
    printf("THERMAL_IDENTITY uid=%u gid=%u\n", getuid(), getgid());
    if (auto *file = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) if (!strncmp(line, "CapEff:", 7)) fputs(line, stdout);
        fclose(file);
    }
    unsigned before = fds();
    auto api = IThermalInterface::Get();
    if (!api) { puts("THERMAL_HDI_CONNECTION_FAILED"); return 2; }
    uint32_t major = 0, minor = 0;
    int result = api->GetVersion(major, minor);
    printf("THERMAL_VERSION result=%d major=%u minor=%u\n", result, major, minor);
    if (result || major != 1 || minor != 1) return 2;
    int cpu = api->SetCpuFreq(1000000), gpu = api->SetGpuFreq(1000000);
    int charge = api->SetBatteryCurrent(500), isolate = api->IsolateCpu(1);
    printf("THERMAL_WRITES_UNSUPPORTED cpu=%d gpu=%d charge=%d isolate=%d\n", cpu, gpu, charge, isolate);
    if (cpu != HDF_ERR_NOT_SUPPORT || gpu != HDF_ERR_NOT_SUPPORT || charge != HDF_ERR_NOT_SUPPORT || isolate != HDF_ERR_NOT_SUPPORT) return 2;
    HdfThermalCallbackInfo current;
    result = api->GetThermalZoneInfo(current);
    printf("THERMAL_GET result=%d count=%zu\n", result, current.info.size());
    for (const auto &sensor : current.info) printf("THERMAL_CURRENT type=%s milli_celsius=%d\n", sensor.type.c_str(), sensor.temp);
    if (absent) {
        if (result != HDF_ERR_NOT_SUPPORT || !current.info.empty()) return 2;
        puts("THERMAL_ABSENT_PASS no_synthetic_reading=1");
        return 0;
    }
    if (result || current.info.empty()) return 2;
    auto &collector = OHOS::HDI::ObjectCollector::GetInstance();
    collector.ConstructorRegister(IThermalCallback::GetDescriptor(),
        [](const sptr<OHOS::HDI::HdiBase> &iface) -> sptr<IRemoteObject> {
            if (!iface) return nullptr;
            return new ThermalCallbackStub(static_cast<IThermalCallback *>(iface.GetRefPtr()));
        });
    sptr<Observer> observer = new Observer;
    result = api->Register(observer);
    printf("THERMAL_REGISTER result=%d fds=%u before=%u\n", result, fds(), before);
    if (result) return 2;
    puts("THERMAL_WATCH_READY");
    sleep(14);
    result = api->Unregister();
    unsigned first = observer->events.load();
    usleep(700000); // Allow an already in-flight callback to finish.
    unsigned quiescent = observer->events.load();
    sleep(1);
    unsigned last = observer->events.load();
    int second = api->Unregister();
    printf("THERMAL_UNREGISTER result=%d first_events=%u quiescent_events=%u final_events=%u second=%d fds=%u\n",
        result, first, quiescent, last, second, fds());
    collector.ConstructorUnRegister(IThermalCallback::GetDescriptor());
    if (result || !first || quiescent != last || second == 0) return 2;
    puts("THERMAL_HDI_REAL_CALLBACK_LIFECYCLE_PASS");
    return 0;
}
