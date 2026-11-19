/* Three concurrent, distinct client targets for the production composer ABI. */
#include "composer_abi.h"
#include "display_type.h"
#include "idisplay_buffer_vdi.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

using namespace AuroraComposer;
namespace HdiBuffer = OHOS::HDI::Display::Buffer::V1_0;

static void check(bool ok, const char *operation)
{
    if (ok) return;
    std::fprintf(stderr, "MULTIDISPLAY_FAIL %s\n", operation);
    std::exit(2);
}

template<typename T> static T symbol(void *library, const char *name)
{
    auto value = reinterpret_cast<T>(dlsym(library, name));
    check(value, name);
    return value;
}

struct Output {
    Mode mode = {};
    std::array<BufferHandle *, 2> buffers{};
    std::atomic<unsigned> ticks{0};
};

int main()
{
    alarm(45);
    auto composer = dlopen("/vendor/lib64/libdisplay_composer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    check(composer, "composer");
    auto buffer = dlopen("/vendor/lib64/libdisplay_buffer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    check(buffer, "allocator");
    auto create = symbol<HdiBuffer::IDisplayBufferVdi *(*)()>(buffer, "CreateDisplayBufferVdi");
    auto destroy = symbol<void (*)(HdiBuffer::IDisplayBufferVdi *)>(buffer, "DestroyDisplayBufferVdi");
    auto plug = symbol<int (*)(Hotplug, void *)>(composer, "RegHotPlugCallback");
    auto modes = symbol<int (*)(uint32_t, std::vector<Mode> *)>(composer, "GetDisplaySupportedModes");
    auto identify = symbol<int (*)(uint32_t, uint8_t &, std::vector<uint8_t> &)>(
        composer, "GetDisplayIdentificationData");
    auto vblank = symbol<int (*)(uint32_t, Vblank, void *)>(composer, "RegDisplayVBlankCallback");
    auto vsync = symbol<int (*)(uint32_t, bool)>(composer, "SetDisplayVsyncEnabled");
    auto client = symbol<int (*)(uint32_t, const BufferHandle *, int32_t)>(composer, "SetDisplayClientBuffer");
    auto commit = symbol<int (*)(uint32_t, int32_t *)>(composer, "Commit");
    auto clear = symbol<int (*)(uint32_t)>(composer, "ClearDisplayClientBuffer");

    unsigned connected = 0;
    auto plugged = [](uint32_t device, bool present, void *context) {
        auto &seen = *static_cast<unsigned *>(context);
        check(present && device == seen, "separate device IDs");
        ++seen;
    };
    check(!plug(plugged, &connected) && connected == 3, "three connected KMS outputs");
    auto allocator = create();
    check(allocator, "allocator factory");
    std::array<Output, 3> outputs;
    std::vector<uint8_t> previous_edid;
    for (unsigned device = 0; device < outputs.size(); ++device) {
        auto &output = outputs[device];
        std::vector<Mode> list;
        check(!modes(device, &list) && list.size() == 1, "mode query");
        uint8_t port = 255;
        std::vector<uint8_t> edid;
        check(!identify(device, port, edid) && port == device && edid.size() >= 128 &&
              edid[0] == 0 && edid[1] == 255 && edid != previous_edid, "connector EDID identity");
        previous_edid.swap(edid);
        output.mode = list.front();
        HdiBuffer::AllocInfo info = {uint32_t(output.mode.width), uint32_t(output.mode.height),
                                     HBM_USE_CPU_READ | HBM_USE_CPU_WRITE, PIXEL_FMT_BGRX_8888, 0};
        for (unsigned slot = 0; slot < output.buffers.size(); ++slot) {
            auto &handle = output.buffers[slot];
            check(!allocator->AllocMem(info, handle) && handle, "scanout DMA-BUF allocation");
            auto pixels = static_cast<unsigned char *>(allocator->Mmap(*handle));
            check(pixels, "scanout DMA-BUF map");
            for (int y = 0; y < handle->height; ++y) {
                for (int x = 0; x < handle->width; ++x) {
                    auto pixel = pixels + size_t(y) * handle->stride + x * 4;
                    pixel[0] = 20 + slot * 50;
                    pixel[1] = 40 + device * 50;
                    pixel[2] = x < handle->width / 2 ? 75 : 135;
                    pixel[3] = 255;
                }
            }
            check(!allocator->FlushCache(*handle) && !allocator->Unmap(*handle), "publish scanout DMA-BUF");
        }
        auto tick = [](unsigned int, uint64_t, void *context) {
            ++static_cast<Output *>(context)->ticks;
        };
        check(!vblank(device, tick, &output) && !vsync(device, true), "per-output vsync");
    }

    std::array<std::thread, 3> workers;
    for (unsigned device = 0; device < outputs.size(); ++device) {
        workers[device] = std::thread([&, device] {
            for (unsigned frame = 0; frame < 20; ++frame) {
                int fence = 17;
                check(!client(device, outputs[device].buffers[frame % 2], -1) &&
                      !commit(device, &fence) && fence == -1, "concurrent KMS commit");
                // Vary one producer's cadence without affecting other CRTCs.
                if (device == 2) usleep(23000);
            }
        });
    }
    for (auto &worker : workers) worker.join();
    for (unsigned device = 0; device < outputs.size(); ++device) {
        check(outputs[device].ticks > 0, "vsync delivery");
        std::printf("MULTIDISPLAY_OUTPUT device=%u frames=20 callbacks=%u mode=%dx%d@%u\n",
                    device, outputs[device].ticks.load(), outputs[device].mode.width,
                    outputs[device].mode.height, outputs[device].mode.freshRate);
        check(!vsync(device, false) && !vblank(device, nullptr, nullptr), "vsync shutdown");
    }
    std::puts("MULTIDISPLAY_VISIBLE");
    std::fflush(stdout);
    sleep(6);
    for (unsigned device = 0; device < outputs.size(); ++device) {
        check(!clear(device), "client detachment");
        for (auto handle : outputs[device].buffers) allocator->FreeMem(*handle);
    }
    destroy(allocator);
    dlclose(composer);
    dlclose(buffer);
    std::puts("MULTIDISPLAY_PASS");
}
