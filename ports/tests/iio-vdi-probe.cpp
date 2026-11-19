/* SPDX-License-Identifier: Apache-2.0
 * Diagnostic uses the published VDI 1.1 ABI via the actual HDF loader.
 * It does not qualify the upper SensorService/NDK chain.
 */
#include "linux-iio.h"
#include "isensor_interface_vdi.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
using namespace OHOS;
using namespace OHOS::HDI::Sensor::V1_1;
using namespace OHOS::HDI::Sensor::V3_0;
class Callback final : public ISensorCallbackVdi {
public:
    std::atomic<unsigned> count{0}, errors{0};
    int32_t OnDataEventVdi(const HdfSensorEventsVdi &event) override
    {
        float lux = -1;
        if (event.dataLen != sizeof(lux) || event.data.size() != sizeof(lux) || event.timestamp <= 0) {
            ++errors; return -1;
        }
        std::memcpy(&lux, event.data.data(), sizeof(lux));
        if (!std::isfinite(lux) || lux < 0) { ++errors; return -1; }
        ++count;
        std::printf("IIO_VDI_EVENT count=%u lux=%.9g timestamp=%lld type=%d id=%d\n", count.load(), lux,
            static_cast<long long>(event.timestamp), event.deviceSensorInfo.sensorType, event.deviceSensorInfo.sensorId);
        return 0;
    }
    int32_t OnDataEvent(const HdfSensorEvents &) override { ++errors; return -1; }
    sptr<IRemoteObject> HandleCallbackDeath() override { return nullptr; }
};
int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("IIO_VDI_PROBE_UID=%u\n", getuid());
    if (getuid() != 2000) return 2;
    auto *object = HdfLoadVdi("libaurora_iio_sensor_vdi.z.so");
    if (!object || !object->vdiBase || HdfGetVdiVersion(object) != 1) return 2;
    auto *driver = reinterpret_cast<WrapperSensorVdi *>(object->vdiBase)->sensorModule;
    int error = 0;
    if (!driver || driver->Init()) { HdfCloseVdi(object); return 2; }
    std::vector<HdfSensorInformationVdi> list;
    int result = driver->GetAllSensorInfo(list);
    std::printf("IIO_VDI_LIST result=%d count=%zu\n", result, list.size());
    if (result || list.size() != 1 || list[0].sensorTypeId != HDF_SENSOR_TYPE_AMBIENT_LIGHT) error = 1;
    sptr<Callback> callback = new Callback;
    if (driver->Enable(1) != HDF_ERR_NOT_SUPPORT || driver->SetOption(5, 1) != HDF_ERR_NOT_SUPPORT ||
        driver->SetBatch(5, 0, 0) != HDF_ERR_INVALID_PARAM) error = 1;
    std::printf("IIO_VDI_NEGATIVE_CHECKS error=%d\n", error);
    if (driver->Register(0, callback) || driver->SetBatch(5, 200000000, 0) || driver->Enable(5)) error = 1;
    std::printf("IIO_VDI_READY error=%d\n", error);
    for (int i = 0; i < 50; ++i) {
        std::vector<AuroraIioChannel> channels;
        if (!AuroraDiscoverIio(channels)) for (const auto &channel : channels) {
            AuroraIioSample sample{};
            int status = AuroraReadIio(channel, sample), read_error = errno;
            std::printf("IIO_NATIVE kind=%s status=%d errno=%d value=%.9g t=%lld\n",
                channel.kind == AuroraIioKind::Light ? "light" : "lid", status, status ? read_error : 0,
                status ? -1 : sample.value, static_cast<long long>(sample.timestamp_ns));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (driver->Disable(5)) error = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Drain a bounded in-flight callback.
    auto stopped = callback->count.load();
    if (driver->Unregister(0, callback)) error = 1;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (callback->count != stopped || callback->errors || !stopped) error = 1;
    std::printf("IIO_VDI_STOPPED count=%u errors=%u final_error=%d\n", stopped, callback->errors.load(), error);
    HdfCloseVdi(object);
    std::printf("IIO_VDI_PROBE_EXIT=%d\n", error);
    return error;
}
