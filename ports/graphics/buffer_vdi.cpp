/* SPDX-License-Identifier: GPL-3.0-only
 * Linear RGB buffer VDI for the fixed HAD ABI. Uses DRM dumb/PRIME, not ION,
 * vendor IOVA, a Mali helper, or a Debian runtime.
 */
#include "include/idisplay_buffer_vdi.h"
#include "include/aurora_drm.h"
#include "include/display_type.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

using namespace OHOS::HDI::Display::Buffer;
using BufferAllocInfo = V1_0::AllocInfo;
using BufferVerifyAllocInfo = V1_0::VerifyAllocInfo;
using Identity = std::pair<uint64_t, uint64_t>;
namespace {
constexpr uint64_t VENDOR_LAYOUT = UINT64_C(0xfffff) << 44;
constexpr uint64_t ALIGN_512 = UINT64_C(1) << 18;
std::mutex metadata_lock;
std::map<Identity, std::map<uint32_t, std::vector<uint8_t>>> metadata;

bool identity(const BufferHandle &handle, Identity &id)
{
    struct stat state;
    if (handle.fd < 0 || fstat(handle.fd, &state)) return false;
    id = {state.st_dev, state.st_ino};
    return true;
}

bool supported(uint32_t width, uint32_t height, uint32_t format, uint64_t usage)
{
    if (!width || !height || width > 16384 || height > 16384 ||
        static_cast<uint64_t>(width) * height * 4 > 256 * 1024 * 1024) return false;
    if (format != PIXEL_FMT_RGBX_8888 && format != PIXEL_FMT_RGBA_8888 &&
        format != PIXEL_FMT_BGRX_8888 && format != PIXEL_FMT_BGRA_8888) return false;
    if (usage & HBM_USE_PROTECTED) return false;
    if ((usage & VENDOR_LAYOUT) && !(usage & (HBM_USE_CPU_READ | HBM_USE_CPU_WRITE))) return false;
    return true;
}

bool valid(const BufferHandle &handle)
{
    return handle.fd >= 0 && handle.width > 0 && handle.height > 0 &&
        handle.size > 0 && handle.stride >= static_cast<int64_t>(handle.width) * 4 &&
        static_cast<int64_t>(handle.stride) * handle.height <= handle.size;
}

int sync_dma(int fd, uint64_t flags)
{
    struct dma_buf_sync request = {flags};
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &request);
}

/* Diagnostic breadcrumbs; both devhost and passthrough clients capture stderr. */
#define AURORA_VDI_LOG(...)                                  \
    do {                                                     \
        fprintf(stderr, "AuroraVDI: " __VA_ARGS__);          \
        fputc('\n', stderr);                                 \
        fflush(stderr);                                      \
    } while (0)

/* Log the direct caller address plus a one-shot /proc/self/maps so each VDI
 * invocation can be attributed to the exact service wrapper library
 * (mapper vs metadata vs allocator client) offline. */
static void log_caller(const char *what, void *ra0)
{
    AURORA_VDI_LOG("%s caller=%p", what, ra0);
    static bool maps_dumped = false;
    if (!maps_dumped) {
        maps_dumped = true;
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                fputs("AuroraVDIMAP: ", stderr);
                fputs(line, stderr);
            }
            fclose(f);
            fflush(stderr);
        }
    }
}

struct Mapping { void *address; size_t size; unsigned int users; };

class BufferVdi final : public V1_0::IDisplayBufferVdi {
    mutable std::mutex lock_;
    mutable AuroraDrmDisplay display_ = {};
    mutable std::map<Identity, Mapping> mappings_;
public:
    BufferVdi() { display_.fd = -1; }
    ~BufferVdi() override
    {
        for (const auto &item : mappings_) munmap(item.second.address, item.second.size);
        if (display_.fd >= 0) close(display_.fd);
    }

    int32_t AllocMem(const BufferAllocInfo &info, BufferHandle *&out) const override
    {
        out = nullptr;
        AURORA_VDI_LOG("AllocMem w=%u h=%u fmt=%d usage=0x%llx expected=%zu",
                       info.width, info.height, static_cast<int>(info.format),
                       static_cast<unsigned long long>(info.usage), info.expectedSize);
        if (!supported(info.width, info.height, info.format, info.usage)) {
            AURORA_VDI_LOG("AllocMem NOT_SUPPORT by validation");
            return NOT_SUPPORT;
        }
        std::lock_guard<std::mutex> guard(lock_);
        if (display_.fd < 0 && AuroraOpenDisplayDrm(&display_, 0)) {
            AURORA_VDI_LOG("AllocMem drm open failed");
            return -1;
        }
        uint32_t allocated_width = info.width;
        if (info.usage & ALIGN_512) allocated_width = (allocated_width + 127) & ~127U;
        struct drm_mode_create_dumb request = {};
        request.width = allocated_width;
        request.height = info.height;
        request.bpp = 32;
        if (ioctl(display_.fd, DRM_IOCTL_MODE_CREATE_DUMB, &request)) {
            AURORA_VDI_LOG("AllocMem CREATE_DUMB errno=%d", errno);
            return -1;
        }
        int fd = -1;
        int ret = drmPrimeHandleToFD(display_.fd, request.handle, DRM_CLOEXEC | DRM_RDWR, &fd);
        struct drm_mode_destroy_dumb destroy = {request.handle};
        (void)ioctl(display_.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        if (ret) {
            AURORA_VDI_LOG("AllocMem PRIME_HANDLE_TO_FD ret=%d errno=%d", ret, errno);
            return -1;
        }
        if (request.size > INT_MAX || request.size < info.expectedSize) {
            AURORA_VDI_LOG("AllocMem size mismatch got=%u expected=%zu",
                           request.size, info.expectedSize);
            close(fd);
            return NOT_SUPPORT;
        }
        BufferHandle *handle = AllocateBufferHandle(0, 0);
        if (!handle) {
            AURORA_VDI_LOG("AllocMem AllocateBufferHandle(0,0) returned null");
            close(fd);
            return -1;
        }
        handle->fd = fd;
        handle->width = info.width;
        handle->height = info.height;
        handle->stride = request.pitch;
        handle->size = request.size;
        handle->format = info.format;
        handle->usage = info.usage & ~VENDOR_LAYOUT;
        handle->virAddr = nullptr;
        handle->phyAddr = 0;
        out = handle;
        AURORA_VDI_LOG("AllocMem OK fd=%d stride=%d size=%u handle=%p",
                       handle->fd, handle->stride, handle->size,
                       static_cast<void *>(handle));
        return 0;
    }

    void FreeMem(const BufferHandle &handle) const override
    {
        Identity id;
        AURORA_VDI_LOG("FreeMem fd=%d", handle.fd);
        log_caller("FreeMem", __builtin_return_address(0));
        if (identity(handle, id)) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                auto found = mappings_.find(id);
                if (found != mappings_.end()) {
                    munmap(found->second.address, found->second.size);
                    mappings_.erase(found);
                }
            }
            std::lock_guard<std::mutex> guard(metadata_lock);
            metadata.erase(id);
        }
        FreeBufferHandle(const_cast<BufferHandle *>(&handle));
    }

    void *Mmap(const BufferHandle &handle) const override
    {
        Identity id;
        AURORA_VDI_LOG("Mmap fd=%d w=%u h=%u size=%u", handle.fd, handle.width,
                       handle.height, handle.size);
        log_caller("Mmap", __builtin_return_address(0));
        if (!valid(handle) || !identity(handle, id)) {
            AURORA_VDI_LOG("Mmap invalid handle");
            return nullptr;
        }
        std::lock_guard<std::mutex> guard(lock_);
        auto found = mappings_.find(id);
        if (found != mappings_.end()) {
            if (found->second.size < static_cast<size_t>(handle.size)) return nullptr;
            ++found->second.users;
            const_cast<BufferHandle &>(handle).virAddr = found->second.address;
            return found->second.address;
        }
        if (sync_dma(handle.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW)) {
            AURORA_VDI_LOG("Mmap sync_dma START failed fd=%d errno=%d", handle.fd, errno);
            return nullptr;
        }
        void *address = mmap(nullptr, handle.size, PROT_READ | PROT_WRITE, MAP_SHARED, handle.fd, 0);
        if (address == MAP_FAILED) {
            AURORA_VDI_LOG("Mmap mmap failed fd=%d size=%u errno=%d", handle.fd,
                           handle.size, errno);
            (void)sync_dma(handle.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
            return nullptr;
        }
        mappings_[id] = {address, static_cast<size_t>(handle.size), 1};
        /* The stock HDI mapper reports status and the client wrapper reads
         * handle.virAddr, rather than forwarding our pointer return value. */
        const_cast<BufferHandle &>(handle).virAddr = address;
        AURORA_VDI_LOG("Mmap OK fd=%d addr=%p", handle.fd, address);
        return address;
    }

    int32_t Unmap(const BufferHandle &handle) const override
    {
        Identity id;
        if (!identity(handle, id)) return -1;
        std::lock_guard<std::mutex> guard(lock_);
        auto found = mappings_.find(id);
        if (found == mappings_.end()) return -1;
        if (--found->second.users) return 0;
        int ret = munmap(found->second.address, found->second.size);
        mappings_.erase(found);
        const_cast<BufferHandle &>(handle).virAddr = nullptr;
        if (sync_dma(handle.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW)) return -1;
        return ret;
    }

    int32_t FlushCache(const BufferHandle &handle) const override
    {
        AURORA_VDI_LOG("FlushCache fd=%d", handle.fd);
        if (sync_dma(handle.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return -1;
        return sync_dma(handle.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    }

    int32_t InvalidateCache(const BufferHandle &handle) const override
    {
        AURORA_VDI_LOG("InvalidateCache fd=%d", handle.fd);
        if (sync_dma(handle.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) return -1;
        return sync_dma(handle.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }

    int32_t IsSupportedAlloc(const std::vector<BufferVerifyAllocInfo> &infos,
                            std::vector<bool> &results) const override
    {
        if (infos.size() > 4096) return -1;
        results.clear();
        results.reserve(infos.size());
        for (const auto &info : infos) {
            bool ok = supported(info.width, info.height, info.format, info.usage);
            AURORA_VDI_LOG("IsSupportedAlloc w=%u h=%u fmt=%d usage=0x%llx -> %d",
                           info.width, info.height, static_cast<int>(info.format),
                           static_cast<unsigned long long>(info.usage), ok);
            results.push_back(ok);
        }
        return 0;
    }

    int32_t RegisterBuffer(const BufferHandle &handle) override
    {
        Identity id;
        AURORA_VDI_LOG("RegisterBuffer fd=%d w=%u h=%u stride=%d size=%u",
                       handle.fd, handle.width, handle.height, handle.stride, handle.size);
        log_caller("RegisterBuffer", __builtin_return_address(0));
        return valid(handle) && identity(handle, id) ? 0 : -1;
    }

    int32_t SetMetadata(const BufferHandle &handle, uint32_t key, const std::vector<uint8_t> &value) override
    {
        Identity id;
        AURORA_VDI_LOG("SetMetadata fd=%d key=%u bytes=%zu", handle.fd, key, value.size());
        if (!identity(handle, id) || value.size() > 65536) return -1;
        std::lock_guard<std::mutex> guard(metadata_lock);
        if (!metadata.count(id) && metadata.size() >= 1024) return -1;
        auto &values = metadata[id];
        if (!values.count(key) && values.size() >= 64) return -1;
        values[key] = value;
        return 0;
    }

    int32_t GetMetadata(const BufferHandle &handle, uint32_t key, std::vector<uint8_t> &value) override
    {
        Identity id;
        if (!identity(handle, id)) return -1;
        std::lock_guard<std::mutex> guard(metadata_lock);
        auto object = metadata.find(id);
        if (object == metadata.end()) {
            AURORA_VDI_LOG("GetMetadata key=%u no-object NOT_SUPPORT", key);
            return NOT_SUPPORT;
        }
        auto item = object->second.find(key);
        if (item == object->second.end()) {
            AURORA_VDI_LOG("GetMetadata key=%u absent NOT_SUPPORT", key);
            return NOT_SUPPORT;
        }
        value = item->second;
        return 0;
    }

    int32_t ListMetadataKeys(const BufferHandle &handle, std::vector<uint32_t> &keys) override
    {
        Identity id;
        if (!identity(handle, id)) return -1;
        std::lock_guard<std::mutex> guard(metadata_lock);
        keys.clear();
        auto object = metadata.find(id);
        if (object != metadata.end())
            for (const auto &item : object->second) keys.push_back(item.first);
        return 0;
    }

    int32_t EraseMetadataKey(const BufferHandle &handle, uint32_t key) override
    {
        Identity id;
        if (!identity(handle, id)) return -1;
        std::lock_guard<std::mutex> guard(metadata_lock);
        auto object = metadata.find(id);
        if (object != metadata.end()) object->second.erase(key);
        return 0;
    }

    int32_t GetImageLayout(const BufferHandle &handle, V1_2::ImageLayout &layout) const override
    {
        AURORA_VDI_LOG("GetImageLayout fd=%d fmt=%d", handle.fd,
                       static_cast<int>(handle.format));
        if (!valid(handle)) return -1;
        layout.pixelFormat = handle.format;
        layout.planes = {{0, 4, static_cast<uint32_t>(handle.stride)}};
        return 0;
    }
};
}

extern "C" V1_0::IDisplayBufferVdi *CreateDisplayBufferVdi()
{
    BufferHandle *abi = AllocateBufferHandle(2, 3);
    if (!abi) return nullptr;
    bool matches = abi->reserveFds == 2 && abi->reserveInts == 3;
    FreeBufferHandle(abi);
    if (!matches) {
        fprintf(stderr, "Aurora allocator: source BufferHandle ABI differs from the fixed 56-byte ABI\n");
        return nullptr;
    }
    return new BufferVdi();
}

extern "C" void DestroyDisplayBufferVdi(V1_0::IDisplayBufferVdi *instance)
{
    delete instance;
}
