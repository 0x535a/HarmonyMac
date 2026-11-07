/* Standalone OHOS caller of the same exported ABI loaded by HDF. */
#include "composer_abi.h"
#include "buffer_handle.h"
#include "display_type.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace AuroraComposer;
static void check(bool ok, const char *what)
{ if (!ok) { fprintf(stderr, "COMPOSER_PROBE_FAIL %s\n", what); exit(2); } }
template<typename T> static T symbol(void *library, const char *name)
{ auto result = reinterpret_cast<T>(dlsym(library, name)); check(result, name); return result; }
static std::atomic<unsigned> ticks{0};
static void tick(unsigned int, uint64_t, void *) { ++ticks; }
static void hotplug(uint32_t device, bool connected, void *data)
{ check(!device && connected, "internal display hotplug"); *static_cast<bool *>(data) = true; }
int main()
{
    setbuf(stdout, nullptr); alarm(45);
    void *library = dlopen("/vendor/lib64/aurora-graphics/libdisplay_composer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { puts(dlerror()); return 2; }
    auto plug = symbol<int (*)(Hotplug, void *)>(library, "RegHotPlugCallback");
    auto capability = symbol<int (*)(uint32_t, Capability *)>(library, "GetDisplayCapability");
    auto modes = symbol<int (*)(uint32_t, std::vector<Mode> *)>(library, "GetDisplaySupportedModes");
    auto create = symbol<int (*)(uint32_t, const AuroraComposer::LayerInfo *, uint32_t *)>(library, "CreateLayer");
    auto destroy = symbol<int (*)(uint32_t, uint32_t)>(library, "DestroyLayer");
    auto composition = symbol<int (*)(uint32_t, uint32_t, int32_t)>(library, "SetLayerCompositionType");
    auto changes = symbol<int (*)(uint32_t, std::vector<uint32_t> *, std::vector<int32_t> *)>(library, "GetDisplayCompChange");
    auto client = symbol<int (*)(uint32_t, const BufferHandle *, int32_t)>(library, "SetDisplayClientBuffer");
    auto crop = symbol<int (*)(uint32_t, const Rect *)>(library, "SetDisplayClientCrop");
    auto clear = symbol<int (*)(uint32_t)>(library, "ClearDisplayClientBuffer");
    auto commit = symbol<int (*)(uint32_t, int32_t *)>(library, "Commit");
    auto vblank = symbol<int (*)(uint32_t, Vblank, void *)>(library, "RegDisplayVBlankCallback");
    auto vsync = symbol<int (*)(uint32_t, bool)>(library, "SetDisplayVsyncEnabled");
    bool connected = false;
    check(!plug(hotplug, &connected) && connected, "hotplug notification");
    Capability cap;
    check(!capability(0, &cap) && cap.supportLayers == 0 && !cap.name.empty() && cap.props.empty(), "platform capability ABI");
    std::vector<Mode> display_modes;
    check(!modes(0, &display_modes) && display_modes.size() == 1 && display_modes[0].width > 0, "platform mode vector ABI");
    printf("COMPOSER_MODE %dx%d@%u %s\n", display_modes[0].width, display_modes[0].height,
           display_modes[0].freshRate, cap.name.c_str());
    AuroraComposer::LayerInfo info = {13, 7, 0, 32, PIXEL_FMT_RGBA_8888};
    uint32_t id;
    check(!create(0, &info, &id) && !composition(0, id, 2), "create cursor-requested layer");
    std::vector<uint32_t> ids; std::vector<int32_t> types;
    check(!changes(0, &ids, &types) && ids.size() == 1 && ids[0] == id && types[0] == 0, "force cursor through CLIENT");
    check(!changes(0, &ids, &types) && ids.empty() && types.empty(), "composition change acknowledgement");
    check(!vblank(0, tick, nullptr) && !vsync(0, true), "enable scheduling callback");
    usleep(100000);
    check(ticks > 0, "scheduling callback delivered");
    check(!vsync(0, false) && !vblank(0, nullptr, nullptr), "disable callback");
    int fence = 17;
    check(commit(0, &fence) != 0 && fence == -1, "empty commit rejected");
    // A small padded target exercises scaling and stride handling. Each final
    // frame has a nonuniform pattern, so captures can verify conversion/crop.
    constexpr int width = 13, height = 7, stride = 64, size = stride * height;
    int fd = memfd_create("aurora-client-test", MFD_CLOEXEC);
    check(fd >= 0 && !ftruncate(fd, size), "client memfd");
    auto pixels = static_cast<unsigned char *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    check(pixels != MAP_FAILED, "client map");
    BufferHandle buffer = {};
    buffer.fd = fd; buffer.width = width; buffer.height = height; buffer.stride = stride;
    buffer.size = size; buffer.format = PIXEL_FMT_RGBA_8888;
    // This intentionally fails DMA_BUF_IOCTL_SYNC: a generic memfd must not
    // silently masquerade as a DMA-BUF accepted by the display backend.
    check(!client(0, &buffer, -1) && commit(0, &fence) != 0, "non-DMA-BUF rejected");
    clear(0); munmap(pixels, size); close(fd);
    // The real allocator supplies PRIME DMA-BUFs using the fixed handle ABI.
    void *allocator_library = dlopen("/vendor/lib64/aurora-graphics/libdisplay_buffer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    check(allocator_library, "allocator library");
    // Keep the C++ VDI call in a separate header to exercise its real vtable.
    extern int present_allocated(void *, int (*)(uint32_t, const BufferHandle *, int32_t),
                                int (*)(uint32_t, const Rect *), int (*)(uint32_t, int32_t *));
    check(!present_allocated(allocator_library, client, crop, commit), "allocated presentations");
    check(!clear(0) && !destroy(0, id), "client/layer cleanup");
    puts("COMPOSER_FULL_PROBE_PASS");
    // dlclose joins the callback thread and restores the previous CRTC.
    dlclose(library); dlclose(allocator_library);
    return 0;
}

#include "idisplay_buffer_vdi.h"
int present_allocated(void *library, int (*client)(uint32_t, const BufferHandle *, int32_t),
                      int (*crop)(uint32_t, const Rect *), int (*commit)(uint32_t, int32_t *))
{
    using Vdi = OHOS::HDI::Display::Buffer::V1_0::IDisplayBufferVdi;
    auto create = symbol<Vdi *(*)()>(library, "CreateDisplayBufferVdi");
    auto destroy = symbol<void (*)(Vdi *)>(library, "DestroyDisplayBufferVdi");
    Vdi *allocator = create(); check(allocator, "allocator factory");
    for (int frame = 0; frame < 4; ++frame) {
        auto format = frame % 2 ? PIXEL_FMT_BGRA_8888 : PIXEL_FMT_RGBA_8888;
        OHOS::HDI::Display::Buffer::V1_0::AllocInfo info = {13, 7, HBM_USE_CPU_READ | HBM_USE_CPU_WRITE, uint32_t(format), 0};
        BufferHandle *buffer = nullptr;
        check(!allocator->AllocMem(info, buffer) && buffer, "DMA-BUF allocation");
        auto pixels = static_cast<unsigned char *>(allocator->Mmap(*buffer));
        check(pixels, "DMA-BUF CPU mapping");
        for (int y = 0; y < buffer->height; ++y) for (int x = 0; x < buffer->width; ++x) {
            auto pixel = pixels + y * buffer->stride + x * 4;
            unsigned char red = x < 6 ? 255 : 0, green = y < 3 ? 255 : 0, blue = 64;
            pixel[0] = frame % 2 ? blue : red; pixel[1] = green;
            pixel[2] = frame % 2 ? red : blue; pixel[3] = 255;
        }
        check(!allocator->FlushCache(*buffer) && !allocator->Unmap(*buffer), "publish DMA-BUF pixels");
        Rect region = frame < 2 ? Rect{0, 0, 13, 7} : Rect{2, 1, 9, 5};
        int fence = 9;
        check(!crop(0, &region) && !client(0, buffer, -1) && !commit(0, &fence) && fence == -1, "KMS frame commit");
        printf("COMPOSER_FRAME_PASS frame=%d format=%d crop=%d,%d,%d,%d\n", frame, format, region.x, region.y, region.w, region.h);
        allocator->FreeMem(*buffer);
    }
    puts("COMPOSER_KMS_VISIBLE quadrants=yellow,green,red,black blue=64 crop=2,1,9,5");
    sleep(10);
    destroy(allocator);
    return 0;
}
