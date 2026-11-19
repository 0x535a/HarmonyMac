/* SPDX-License-Identifier: Apache-2.0
 * Real IIO ambient light for the stock Sensor HDI 3.0 / VDI 1.1 wrapper.
 * LAS degrees remain a native reading; they are not fabricated Hall/posture.
 */
#include "linux-iio.h"
#include "isensor_interface_vdi.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

using namespace OHOS;
using namespace OHOS::HDI::Sensor::V1_1;
using namespace OHOS::HDI::Sensor::V3_0;
namespace {
constexpr int LIGHT = HDF_SENSOR_TYPE_AMBIENT_LIGHT;
constexpr int64_t MIN_INTERVAL = 200000000, MAX_INTERVAL = 10000000000LL;
SensorHandle lightHandle()
{
    SensorHandle h{};
    h.deviceId = DEFAULT_DEVICE_ID; h.sensorType = LIGHT;
    h.sensorId = DEFAULT_SENSOR_ID; h.location = DEFAULT_LOCATION;
    return h;
}
bool valid(const SensorHandle &h)
{
    auto want = lightHandle();
    return h.deviceId == want.deviceId && h.sensorType == want.sensorType &&
        h.sensorId == want.sensorId && h.location == want.location;
}
int light(AuroraIioChannel &selected)
{
    std::vector<AuroraIioChannel> channels;
    if (AuroraDiscoverIio(channels)) return -1;
    auto count = std::count_if(channels.begin(), channels.end(), [](const auto &c) { return c.kind == AuroraIioKind::Light; });
    // Do not change sensor identity by arbitrarily selecting one of many ALSes.
    if (count != 1) { errno = count ? ENOTUNIQ : ENODEV; return -1; }
    selected = *std::find_if(channels.begin(), channels.end(), [](const auto &c) { return c.kind == AuroraIioKind::Light; });
    return 0;
}
class IioSensor final : public ISensorInterfaceVdi {
public:
    ~IioSensor() override
    {
        { std::lock_guard<std::mutex> l(mutex_); stopping_ = true; enabled_ = false; callbacks_.clear(); }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    int32_t Init() override
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!thread_.joinable()) thread_ = std::thread([this] { poll(); });
        return HDF_SUCCESS;
    }
    int32_t GetAllSensorInfo(std::vector<HdfSensorInformationVdi> &info) override
    {
        info.clear();
        AuroraIioChannel channel{};
        if (light(channel)) return errno == ENODEV ? HDF_SUCCESS : HDF_ERR_NOT_SUPPORT;
        HdfSensorInformationVdi entry{};
        entry.sensorName = channel.name; entry.vendorName = "Linux IIO";
        entry.firmwareVersion = "unknown"; entry.hardwareVersion = "unknown";
        entry.sensorTypeId = LIGHT; entry.sensorId = LIGHT; entry.sensorHandle = lightHandle();
        // This is the supported software numeric range, not a datasheet claim.
        entry.maxRange = 2147483648.0f;
        // Physical accuracy/power are unknown, not invented measurements.
        entry.accuracy = 0; entry.power = 0;
        // The installed wrapper forwards these unchanged into SensorService's
        // *PeriodNs and public NDK *SamplingInterval. Target evidence uses ns.
        entry.minDelay = MIN_INTERVAL; entry.maxDelay = MAX_INTERVAL;
        info.push_back(entry);
        return HDF_SUCCESS;
    }
    int32_t GetDeviceSensorInfo(int32_t device, std::vector<HdfSensorInformationVdi> &info) override
    {
        info.clear();
        return device == DEFAULT_DEVICE_ID ? GetAllSensorInfo(info) : HDF_ERR_NOT_SUPPORT;
    }
    int32_t Enable(int32_t id) override
    {
        if (id != LIGHT) return HDF_ERR_NOT_SUPPORT;
        AuroraIioChannel channel{};
        if (light(channel)) return HDF_ERR_NOT_SUPPORT;
        { std::lock_guard<std::mutex> l(mutex_); channel_ = channel; enabled_ = true; have_last_ = false; ++generation_; }
        condition_.notify_all(); return HDF_SUCCESS;
    }
    int32_t Disable(int32_t id) override
    {
        if (id != LIGHT) return HDF_ERR_NOT_SUPPORT;
        std::lock_guard<std::mutex> l(mutex_); enabled_ = false; ++generation_; return HDF_SUCCESS;
    }
    int32_t SetBatch(int32_t id, int64_t sampling, int64_t reporting) override
    {
        if (id != LIGHT) return HDF_ERR_NOT_SUPPORT;
        if (sampling < MIN_INTERVAL || sampling > MAX_INTERVAL || reporting < 0) return HDF_ERR_INVALID_PARAM;
        if (reporting > sampling) return HDF_ERR_NOT_SUPPORT; // No batching/FIFO advertised.
        { std::lock_guard<std::mutex> l(mutex_); interval_ = sampling; ++generation_; }
        condition_.notify_all(); return HDF_SUCCESS;
    }
    int32_t SetSaBatch(int32_t id, int64_t sampling, int64_t reporting) override
    {
        if (id != LIGHT) return HDF_ERR_NOT_SUPPORT;
        if (sampling < 0 || reporting < 0) return HDF_ERR_INVALID_PARAM;
        // The stock wrapper already applies the aggregate/clamped SetBatch.
        // SA-only votes (including an inactive INT64_MAX vote) are hints for
        // MCU/FIFO backends. IIO has no second SA hardware programming path.
        return HDF_SUCCESS;
    }
    int32_t SetMode(int32_t id, int32_t mode) override
    {
        if (id != LIGHT || mode < 0 || mode > 2) return HDF_ERR_NOT_SUPPORT;
        std::lock_guard<std::mutex> l(mutex_); mode_ = mode; have_last_ = false; ++generation_; return HDF_SUCCESS;
    }
    int32_t SetOption(int32_t id, uint32_t option) override { return id == LIGHT && option == 0 ? HDF_SUCCESS : HDF_ERR_NOT_SUPPORT; }
    int32_t GetSdcSensorInfo(std::vector<SdcSensorInfoVdi> &info) override { info.clear(); return HDF_ERR_NOT_SUPPORT; }
    int32_t Register(int32_t group, const sptr<ISensorCallbackVdi> &callback) override
    {
        if (group != HDF_TRADITIONAL_SENSOR_TYPE || !callback) return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> l(mutex_);
        if (std::find(callbacks_.begin(), callbacks_.end(), callback) != callbacks_.end()) return HDF_ERR_DEVICE_BUSY;
        if (callbacks_.size() >= 16) return HDF_ERR_DEVICE_BUSY;
        callbacks_.push_back(callback); have_last_ = false; return HDF_SUCCESS;
    }
    int32_t Unregister(int32_t group, const sptr<ISensorCallbackVdi> &callback) override
    {
        if (group != HDF_TRADITIONAL_SENSOR_TYPE || !callback) return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> l(mutex_);
        auto it = std::find(callbacks_.begin(), callbacks_.end(), callback);
        if (it == callbacks_.end()) return HDF_ERR_INVALID_OBJECT;
        callbacks_.erase(it); return HDF_SUCCESS;
    }
    int32_t Enable(SensorHandle h) override { return valid(h) ? Enable(LIGHT) : HDF_ERR_NOT_SUPPORT; }
    int32_t Disable(SensorHandle h) override { return valid(h) ? Disable(LIGHT) : HDF_ERR_NOT_SUPPORT; }
    int32_t SetBatch(SensorHandle h, int64_t a, int64_t b) override { return valid(h) ? SetBatch(LIGHT, a, b) : HDF_ERR_NOT_SUPPORT; }
    int32_t SetSaBatch(SensorHandle h, int64_t a, int64_t b) override { return valid(h) ? SetSaBatch(LIGHT, a, b) : HDF_ERR_NOT_SUPPORT; }
    int32_t SetMode(SensorHandle h, int32_t m) override { return valid(h) ? SetMode(LIGHT, m) : HDF_ERR_NOT_SUPPORT; }
    int32_t SetOption(SensorHandle h, uint32_t o) override { return valid(h) ? SetOption(LIGHT, o) : HDF_ERR_NOT_SUPPORT; }
    int32_t RegSensorPlugCallBack(const sptr<ISensorPlugCallback> &) override { return HDF_ERR_NOT_SUPPORT; }
    int32_t UnRegSensorPlugCallBack(const sptr<ISensorPlugCallback> &) override { return HDF_ERR_NOT_SUPPORT; }
private:
    void poll()
    {
        std::unique_lock<std::mutex> l(mutex_);
        while (!stopping_) {
            condition_.wait_for(l, std::chrono::nanoseconds(interval_));
            if (stopping_) break;
            if (!enabled_ || callbacks_.empty()) continue;
            auto channel = channel_;
            auto generation = generation_;
            l.unlock();
            AuroraIioSample sample{};
            int status = AuroraReadIio(channel, sample);
            l.lock();
            if (status || !enabled_ || generation != generation_) continue;
            float lux = static_cast<float>(sample.value);
            if (mode_ == 2 && have_last_ && last_ == lux) continue;
            have_last_ = true; last_ = lux;
            HdfSensorEventsVdi event{};
            event.sensorId = LIGHT; event.deviceSensorInfo = lightHandle(); event.version = 1;
            event.timestamp = sample.timestamp_ns; event.mode = mode_; event.dataLen = sizeof(lux);
            event.data.resize(sizeof(lux)); std::memcpy(event.data.data(), &lux, sizeof(lux));
            auto callbacks = callbacks_;
            for (const auto &callback : callbacks) {
                if (!enabled_ || generation != generation_) break;
                if (std::find(callbacks_.begin(), callbacks_.end(), callback) == callbacks_.end()) continue;
                l.unlock();
                int result = callback->OnDataEventVdi(event);
                l.lock();
                if (result) callbacks_.erase(std::remove(callbacks_.begin(), callbacks_.end(), callback), callbacks_.end());
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    bool stopping_ = false, enabled_ = false, have_last_ = false;
    float last_ = 0;
    int32_t mode_ = 2;
    int64_t interval_ = MIN_INTERVAL;
    uint64_t generation_ = 0;
    AuroraIioChannel channel_{};
    std::vector<sptr<ISensorCallbackVdi>> callbacks_;
};
int create(HdfVdiBase *base)
{
    if (!base) return HDF_ERR_INVALID_PARAM;
    auto *wrapper = reinterpret_cast<WrapperSensorVdi *>(base);
    if (wrapper->sensorModule) return HDF_ERR_DEVICE_BUSY;
    wrapper->sensorModule = new (std::nothrow) IioSensor;
    return wrapper->sensorModule ? HDF_SUCCESS : HDF_ERR_MALLOC_FAIL;
}
int destroy(HdfVdiBase *base)
{
    if (!base) return HDF_ERR_INVALID_PARAM;
    auto *wrapper = reinterpret_cast<WrapperSensorVdi *>(base);
    delete wrapper->sensorModule; wrapper->sensorModule = nullptr;
    return HDF_SUCCESS;
}
WrapperSensorVdi module{{1, "aurora_iio_sensor", create, destroy}, nullptr};
}
extern "C" { HDF_VDI_INIT(module); }
