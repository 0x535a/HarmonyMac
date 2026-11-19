/* SPDX-License-Identifier: GPL-3.0-only
 * RenderService-only format negotiation for its stock FrameBuffer producer.
 */
#include "include/display_type.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace {
using Create = void *(*)(void *);
using GetId = int (*)(void *, uint64_t *);
using HandleOpt = int (*)(void *, int, ...);
using GetName = std::string (*)(void *);
// OpenHarmony 6.0 inner_api/surface/external_window.h, NativeWindowOperation.
constexpr int SET_FORMAT = 3;
std::atomic<uint64_t> framebuffer_id{0};

struct SurfaceApi {
    void *lib = dlopen("/system/lib64/chipset-sdk-sp/libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
    Create create = lib ? reinterpret_cast<Create>(dlsym(lib, "OH_NativeWindow_CreateNativeWindow")) : nullptr;
    GetId get_id = lib ? reinterpret_cast<GetId>(dlsym(lib, "OH_NativeWindow_GetSurfaceId")) : nullptr;
    HandleOpt option = lib ? reinterpret_cast<HandleOpt>(dlsym(lib, "OH_NativeWindow_NativeWindowHandleOpt")) : nullptr;
    GetName name = lib ? reinterpret_cast<GetName>(dlsym(lib, "_ZN4OHOS15ProducerSurface7GetNameEv")) : nullptr;
    const void *vtable = lib ? dlsym(lib, "_ZTVN4OHOS15ProducerSurfaceE") : nullptr;
};

SurfaceApi &surface_api()
{
    static SurfaceApi api;
    return api;
}

bool is_stock_framebuffer(SurfaceApi &api, const void *surface_reference)
{
    if (!surface_reference || !api.vtable || !api.name) return false;
    // The public creation API accepts a pointer to an OHOS sptr<Surface>.
    // Read its first member without taking or releasing a stock reference.
    // Only the exact ProducerSurface vtable may use the concrete GetName.
    void *surface = nullptr;
    std::memcpy(&surface, surface_reference, sizeof(surface));
    if (!surface) return false;
    const void *vptr = nullptr;
    std::memcpy(&vptr, surface, sizeof(vptr));
    const auto *address_point = static_cast<const unsigned char *>(api.vtable) + 2 * sizeof(void *);
    return vptr == address_point && api.name(surface) == "FrameBuffer";
}
}

extern "C" void *CreateNativeWindowFromSurface(void *surface_reference)
{
    auto &api = surface_api();
    if (!api.create) return nullptr;
    void *window = api.create(surface_reference);
    uint64_t id;
    if (window && api.get_id && api.option && is_stock_framebuffer(api, surface_reference) &&
        !api.get_id(window, &id) && id) {
        if (!api.option(window, SET_FORMAT, static_cast<int>(PIXEL_FMT_BGRA_8888))) {
            framebuffer_id.store(id, std::memory_order_release);
            std::fprintf(stderr, "AURORA_FRAMEBUFFER_FORMAT configured=BGRA8888\n");
        }
    }
    return window;
}

extern "C" void *OH_NativeWindow_CreateNativeWindow(void *surface_reference)
{
    return CreateNativeWindowFromSurface(surface_reference);
}

extern "C" void AuroraConfigureScanoutWindow(void *window)
{
    const uint64_t expected = framebuffer_id.load(std::memory_order_acquire);
    if (!expected) return;
    auto &api = surface_api();
    uint64_t id;
    if (!api.get_id(window, &id) && id == expected)
        api.option(window, SET_FORMAT, static_cast<int>(PIXEL_FMT_BGRA_8888));
}
