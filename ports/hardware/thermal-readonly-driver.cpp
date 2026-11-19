/* SPDX-License-Identifier: Apache-2.0
 * Real Linux temperature telemetry via the standard Thermal HDI 1.1 RPC.
 * Actuator methods are explicitly unsupported: no simulated mitigation.
 */
#include "linux-thermal.h"
#include "v1_1/thermal_interface_stub.h"
#include <hdf_base.h>
#include <hdf_device_desc.h>
#include <hdf_sbuf_ipc.h>
#include <ipc_skeleton.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <map>
#include <new>
#include <thread>

using namespace OHOS;
using namespace OHOS::HDI::Thermal::V1_1;

namespace {
class LinuxThermal final : public IThermalInterface {
public:
    LinuxThermal() : worker_([this] { poll(); }) {}
    ~LinuxThermal() override
    {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; callbacks_.clear(); }
        condition_.notify_all();
        worker_.join();
    }
    int32_t SetCpuFreq(int32_t) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t SetGpuFreq(int32_t) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t SetBatteryCurrent(int32_t) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t IsolateCpu(int32_t) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t RegisterFanCallback(const sptr<IFanCallback>&) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t UnregisterFanCallback() override { return HDF_ERR_NOT_SUPPORT; }

    int32_t GetThermalZoneInfo(HdfThermalCallbackInfo &event) override
    {
        event.info.clear();
        std::vector<AuroraTemperature> samples;
        if (AuroraReadTemperatures(samples)) return errno == ENODEV ? HDF_ERR_NOT_SUPPORT : HDF_ERR_IO;
        for (const auto &sample : samples) event.info.push_back({sample.source, sample.milli_celsius});
        return HDF_SUCCESS;
    }
    int32_t Register(const sptr<IThermalCallback> &callback) override
    {
        if (!callback) return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t owner = IPCSkeleton::GetCallingPid();
        if (callbacks_.count(owner) || callbacks_.size() >= 16) return HDF_ERR_DEVICE_BUSY;
        callbacks_[owner] = {callback, ++generation_};
        fprintf(stderr, "AURORA_THERMAL_SUBSCRIBE pid=%d uid=%u observers=%zu\n", owner,
            IPCSkeleton::GetCallingUid(), callbacks_.size());
        condition_.notify_all();
        return HDF_SUCCESS;
    }
    int32_t Unregister() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = callbacks_.find(IPCSkeleton::GetCallingPid());
        if (found == callbacks_.end()) return HDF_ERR_INVALID_OBJECT;
        callbacks_.erase(found);
        return HDF_SUCCESS;
    }
private:
    void poll()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            condition_.wait_for(lock, std::chrono::milliseconds(500));
            if (stopping_) break;
            auto callbacks = callbacks_;
            if (callbacks.empty()) continue;
            lock.unlock();
            HdfThermalCallbackInfo event;
            int result = GetThermalZoneInfo(event);
            // A missing/unreadable sensor never produces invented data.
            lock.lock();
            if (result) continue;
            for (const auto &item : callbacks) {
                auto live = callbacks_.find(item.first);
                if (live == callbacks_.end() || live->second.generation != item.second.generation) continue;
                lock.unlock();
                int status = item.second.callback->OnThermalDataEvent(event);
                lock.lock();
                live = callbacks_.find(item.first);
                // A disconnected reader cannot retain another reader's slot.
                if (status && live != callbacks_.end() && live->second.generation == item.second.generation)
                    callbacks_.erase(live);
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    struct Subscription { sptr<IThermalCallback> callback; uint64_t generation; };
    std::map<int32_t, Subscription> callbacks_;
    uint64_t generation_ = 0;
    std::thread worker_;
};

struct Host {
    IDeviceIoService io;
    sptr<IRemoteObject> stub;
};
int32_t dispatch(HdfDeviceIoClient *client, int command, HdfSBuf *data, HdfSBuf *reply)
{
    if (!client || !client->device || !client->device->service) return HDF_ERR_INVALID_PARAM;
    auto *host = CONTAINER_OF(client->device->service, Host, io);
    MessageParcel *input = nullptr, *output = nullptr;
    MessageOption option;
    if (SbufToParcel(data, &input) || SbufToParcel(reply, &output) || !input || !output) return HDF_ERR_INVALID_PARAM;
    return host->stub->SendRequest(command, *input, *output, option);
}
int bind(HdfDeviceObject *device)
{
    if (!device) return HDF_ERR_INVALID_PARAM;
    auto *host = new (std::nothrow) Host{};
    if (!host) return HDF_ERR_MALLOC_FAIL;
    sptr<IThermalInterface> service = new (std::nothrow) LinuxThermal;
    if (!service) { delete host; return HDF_ERR_MALLOC_FAIL; }
    // Construct the generated RPC stub directly with its typed interface.
    // No cross-DSO ObjectCollector registration is required for this bind.
    host->stub = new (std::nothrow) ThermalInterfaceStub(service);
    if (!host->stub) { delete host; return HDF_ERR_MALLOC_FAIL; }
    host->io.Dispatch = dispatch;
    device->service = &host->io;
    fprintf(stderr, "AURORA_THERMAL_READONLY_BOUND version=1.1 real_linux_class=1 actuator_writes=0\n");
    return HDF_SUCCESS;
}
int initialize(HdfDeviceObject *) { return HDF_SUCCESS; }
void release(HdfDeviceObject *device)
{
    if (!device || !device->service) return;
    delete CONTAINER_OF(device->service, Host, io);
    device->service = nullptr;
}
HdfDriverEntry entry = {
    .moduleVersion = 1,
    .moduleName = "libaurora_thermal_driver.z.so",
    .Bind = bind,
    .Init = initialize,
    .Release = release,
};
}
extern "C" { HDF_INIT(entry); }
