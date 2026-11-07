/* SPDX-License-Identifier: GPL-3.0-only
 * Single-display client-composition backend. RenderService composes the full
 * client target; this library copies it to persistent linear KMS buffers.
 * No vendor GPU process, ION, fbdev or Linux desktop session is used.
 */
#include "include/composer_abi.h"
#include "include/buffer_handle.h"
#include "include/display_type.h"
#include "include/aurora_drm.h"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

using namespace AuroraComposer;
namespace {
constexpr int OK = 0, FAIL = -1, PARAM = -3, UNSUPPORTED = -5;
struct Scanout { int dma_fd = -1; uint32_t gem = 0, fb = 0, pitch = 0; uint64_t size = 0; void *map = MAP_FAILED; };
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    AuroraDrmDisplay drm = {};
    Scanout buffers[2];
    drmModeCrtc *saved = nullptr;
    int front = -1, pending_slot = -1, client_fd = -1;
    bool pending = false, enabled = false, stop = false;
    BufferHandle client = {};
    Rect crop = {};
    std::map<uint32_t, int32_t> layers;
    uint32_t next_layer = 1;
    Vblank vblank = nullptr;
    void *vblank_data = nullptr;
    std::thread ticker;
    State() { drm.fd = -1; }
    ~State();
} state;

uint64_t period()
{
    const auto &m = state.drm.mode;
    if (m.clock && m.htotal && m.vtotal) {
        uint64_t ns = UINT64_C(1000000) * m.htotal * m.vtotal / m.clock;
        if (ns >= 1000000 && ns <= 1000000000) return ns;
    }
    return UINT64_C(1000000000) / (m.vrefresh ? m.vrefresh : 60);
}

void release(Scanout &buffer)
{
    if (buffer.map != MAP_FAILED) munmap(buffer.map, buffer.size);
    if (buffer.dma_fd >= 0) close(buffer.dma_fd);
    if (buffer.fb) drmModeRmFB(state.drm.fd, buffer.fb);
    if (buffer.gem) {
        drm_mode_destroy_dumb request = {buffer.gem};
        ioctl(state.drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &request);
    }
    buffer = Scanout{};
}

int initialize()
{
    if (state.drm.fd >= 0) return OK;
    if (AuroraOpenDisplayDrm(&state.drm, 1)) return FAIL;
    state.saved = drmModeGetCrtc(state.drm.fd, state.drm.crtc);
    for (auto &buffer : state.buffers) {
        drm_mode_create_dumb request = {};
        request.width = state.drm.mode.hdisplay;
        request.height = state.drm.mode.vdisplay;
        request.bpp = 32;
        if (ioctl(state.drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &request)) goto fail;
        buffer.gem = request.handle; buffer.pitch = request.pitch; buffer.size = request.size;
        if (drmPrimeHandleToFD(state.drm.fd, buffer.gem, DRM_CLOEXEC | DRM_RDWR, &buffer.dma_fd)) goto fail;
        {
            uint32_t handles[4] = {buffer.gem}, pitches[4] = {buffer.pitch}, offsets[4] = {};
            if (drmModeAddFB2(state.drm.fd, request.width, request.height, DRM_FORMAT_XRGB8888,
                              handles, pitches, offsets, &buffer.fb, 0)) goto fail;
        }
        {
            drm_mode_map_dumb map = {};
            map.handle = buffer.gem;
            if (ioctl(state.drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) goto fail;
            buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, state.drm.fd, map.offset);
            if (buffer.map == MAP_FAILED) goto fail;
            memset(buffer.map, 0, buffer.size);
        }
    }
    return OK;
fail:
    for (auto &buffer : state.buffers) release(buffer);
    if (state.saved) drmModeFreeCrtc(state.saved);
    state.saved = nullptr;
    close(state.drm.fd); state.drm.fd = -1;
    return FAIL;
}

void flipped(int, unsigned int, unsigned int, unsigned int, void *)
{
    state.front = state.pending_slot;
    state.pending = false;
}

int wait_flip()
{
    // A timeout leaves the slot pending. No subsequent commit can write it
    // until its event is drained; never recycle a possibly scanned buffer.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (state.pending) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { errno = ETIMEDOUT; return FAIL; }
        pollfd fd = {state.drm.fd, POLLIN, 0};
        int result = poll(&fd, 1, static_cast<int>(left));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(fd.revents & POLLIN)) return FAIL;
        drmEventContext events = {};
        events.version = 2; events.page_flip_handler = flipped;
        if (drmHandleEvent(state.drm.fd, &events)) return FAIL;
    }
    return OK;
}

int wait_fence(int fence)
{
    if (fence < 0) return OK;
    pollfd fd = {fence, POLLIN, 0};
    int result;
    do { result = poll(&fd, 1, 3000); } while (result < 0 && errno == EINTR);
    return result > 0 && (fd.revents & POLLIN) && !(fd.revents & (POLLERR | POLLNVAL)) ? OK : FAIL;
}

bool valid(const BufferHandle &buffer)
{
    return buffer.fd >= 0 && buffer.width > 0 && buffer.height > 0 &&
        buffer.width <= 16384 && buffer.height <= 16384 &&
        buffer.stride >= int64_t(buffer.width) * 4 && buffer.size > 0 &&
        int64_t(buffer.stride) * buffer.height <= buffer.size &&
        !(buffer.usage & HBM_USE_PROTECTED) &&
        (buffer.format == PIXEL_FMT_RGBA_8888 || buffer.format == PIXEL_FMT_RGBX_8888 ||
         buffer.format == PIXEL_FMT_BGRA_8888 || buffer.format == PIXEL_FMT_BGRX_8888);
}

int copy_client(Scanout &output)
{
    auto &client = state.client;
    Rect crop = state.crop;
    if (!crop.w && !crop.h) crop = {0, 0, client.width, client.height};
    if (crop.x < 0 || crop.y < 0 || crop.w <= 0 || crop.h <= 0 ||
        int64_t(crop.x) + crop.w > client.width || int64_t(crop.y) + crop.h > client.height) return PARAM;
    void *source = mmap(nullptr, client.size, PROT_READ, MAP_SHARED, state.client_fd, 0);
    if (source == MAP_FAILED) return FAIL;
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(state.client_fd, DMA_BUF_IOCTL_SYNC, &sync)) { munmap(source, client.size); return FAIL; }
    dma_buf_sync output_sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (ioctl(output.dma_fd, DMA_BUF_IOCTL_SYNC, &output_sync)) {
        const int saved_errno = errno;
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        (void)ioctl(state.client_fd, DMA_BUF_IOCTL_SYNC, &sync);
        munmap(source, client.size); errno = saved_errno; return FAIL;
    }
    bool rgba = client.format == PIXEL_FMT_RGBA_8888 || client.format == PIXEL_FMT_RGBX_8888;
    static unsigned frame=0;
    bool trace=std::getenv("AURORA_GRAPHICS_TRACE") && (++frame<=5 || frame%120==0);
    uint64_t nonblack=0;
    for (uint32_t y = 0; y < state.drm.mode.vdisplay; ++y) {
        auto src = static_cast<const uint8_t *>(source) +
            (crop.y + uint64_t(y) * crop.h / state.drm.mode.vdisplay) * client.stride;
        auto dst = static_cast<uint8_t *>(output.map) + size_t(y) * output.pitch;
        for (uint32_t x = 0; x < state.drm.mode.hdisplay; ++x) {
            const auto pixel = src + (crop.x + uint64_t(x) * crop.w / state.drm.mode.hdisplay) * 4;
            dst[4*x] = pixel[rgba ? 2 : 0]; dst[4*x+1] = pixel[1];
            dst[4*x+2] = pixel[rgba ? 0 : 2]; dst[4*x+3] = 255;
            if (trace && (pixel[0]||pixel[1]||pixel[2])) ++nonblack;
        }
    }
    if (trace) {
        timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t mono_ms = uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
        std::fprintf(stderr,"AURORA_COMPOSER_FRAME n=%u mono_ms=%llu source=%dx%d format=%d crop=%d,%d,%d,%d nonblack=%llu\n",
            frame, static_cast<unsigned long long>(mono_ms), client.width,client.height,client.format,crop.x,crop.y,crop.w,crop.h,
            static_cast<unsigned long long>(nonblack));
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    int result = ioctl(state.client_fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(source, client.size);
    // Publish CPU writes through the exporter's dma-buf cache contract before
    // handing this reused GEM buffer to KMS. msync is not supported by every
    // DRM mmap implementation and does not express DMA ownership transitions.
    output_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    int output_result = ioctl(output.dma_fd, DMA_BUF_IOCTL_SYNC, &output_sync);
    return result || output_result ? FAIL : OK;
}

void ticker()
{
    std::unique_lock<std::mutex> lock(state.mutex);
    unsigned sequence = 0;
    while (!state.stop) {
        state.wake.wait(lock, [] { return state.stop || (state.enabled && state.vblank); });
        if (state.stop) break;
        auto next = std::chrono::steady_clock::now() + std::chrono::nanoseconds(period());
        if (state.wake.wait_until(lock, next, [] { return state.stop || !state.enabled || !state.vblank; })) continue;
        auto callback = state.vblank; auto data = state.vblank_data;
        timespec timestamp; clock_gettime(CLOCK_MONOTONIC, &timestamp);
        uint64_t ns = uint64_t(timestamp.tv_sec) * 1000000000 + timestamp.tv_nsec;
        lock.unlock(); callback(++sequence, ns, data); lock.lock();
    }
}

State::~State()
{
    { std::lock_guard<std::mutex> lock(mutex); stop = true; wake.notify_all(); }
    if (ticker.joinable()) ticker.join();
    if (drm.fd >= 0) {
        if (saved) {
            drmModeSetCrtc(drm.fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                          saved->mode_valid ? &drm.connector : nullptr, saved->mode_valid ? 1 : 0,
                          saved->mode_valid ? &saved->mode : nullptr);
            drmModeFreeCrtc(saved);
        }
        for (auto &buffer : buffers) release(buffer);
        close(drm.fd);
    }
    if (client_fd >= 0) close(client_fd);
}
bool layer(uint32_t device, uint32_t id) { return device == 0 && state.layers.count(id); }
}

#define API extern "C" __attribute__((visibility("default"))) int32_t
API RegHotPlugCallback(Hotplug callback, void *data)
{
    fprintf(stderr, "AURORA_COMPOSER_HOTPLUG_REGISTER callback=%p\n", reinterpret_cast<void *>(callback));
    if (!callback) return PARAM;
    { std::lock_guard<std::mutex> lock(state.mutex); if (initialize()) return FAIL; }
    callback(0, true, data);
    fprintf(stderr, "AURORA_COMPOSER_HOTPLUG_SENT device=0 connected=1\n");
    return OK;
}
API GetDisplayCapability(uint32_t device, Capability *out)
{
    fprintf(stderr, "AURORA_COMPOSER_CAPABILITY device=%u\n", device);
    if (device || !out) return PARAM;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (initialize()) return FAIL;
    out->name = "Aurora internal display"; out->type = 1;
    out->phyWidth = state.drm.mm_width; out->phyHeight = state.drm.mm_height;
    out->supportLayers = 0; out->virtualDispCount = 0; out->supportWriteBack = false;
    out->propertyCount = 0; out->props.clear(); return OK;
}
API GetDisplaySupportedModes(uint32_t device, std::vector<Mode> *out)
{
    fprintf(stderr, "AURORA_COMPOSER_MODES device=%u\n", device);
    if (device || !out) return PARAM;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (initialize()) return FAIL;
    *out = {{state.drm.mode.hdisplay, state.drm.mode.vdisplay,
             static_cast<uint32_t>((1000000000ULL + period()/2) / period()), 0}};
    return OK;
}
API GetDisplayMode(uint32_t device, uint32_t *mode) { if (device || !mode) return PARAM; *mode = 0; return OK; }
API SetDisplayMode(uint32_t device, uint32_t mode) { return device || mode ? PARAM : OK; }
API GetDisplayPowerStatus(uint32_t device, int32_t *status) { if (device || !status) return PARAM; *status = 0; return OK; }
API SetDisplayPowerStatus(uint32_t device, int32_t status) { return device ? PARAM : (status == 0 ? OK : UNSUPPORTED); }
API GetDisplayBacklight(uint32_t, uint32_t *) { return UNSUPPORTED; }
API SetDisplayBacklight(uint32_t, uint32_t) { return UNSUPPORTED; }
API GetDisplayProperty(uint32_t, uint32_t, uint64_t *) { return UNSUPPORTED; }
API SetDisplayProperty(uint32_t, uint32_t, uint64_t) { return UNSUPPORTED; }
API GetDisplaySupportedColorGamuts(uint32_t device, std::vector<int32_t> *out)
{ if (device || !out) return PARAM; *out = {4}; return OK; }
API CreateLayer(uint32_t device, const AuroraComposer::LayerInfo *info, uint32_t *id)
{
    if (device || !info || !id || info->width <= 0 || info->height <= 0) return PARAM;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.layers.size() >= 128) return FAIL;
    *id = state.next_layer++; state.layers[*id] = -1; return OK;
}
API DestroyLayer(uint32_t device, uint32_t id)
{ std::lock_guard<std::mutex> lock(state.mutex); return !device && state.layers.erase(id) ? OK : PARAM; }
API PrepareDisplayLayers(uint32_t device, bool *flush) { if (device || !flush) return PARAM; *flush = true; return OK; }
API GetDisplayCompChange(uint32_t device, std::vector<uint32_t> *ids, std::vector<int32_t> *types)
{
    if (device || !ids || !types) return PARAM;
    std::lock_guard<std::mutex> lock(state.mutex);
    ids->clear(); types->clear();
    for (auto &item : state.layers) if (item.second != 0) {
        ids->push_back(item.first); types->push_back(0); item.second = 0;
    }
    return OK;
}
API SetLayerCompositionType(uint32_t device, uint32_t id, int32_t type)
{ std::lock_guard<std::mutex> lock(state.mutex); if (!layer(device, id)) return PARAM; state.layers[id] = type; return OK; }
API SetDisplayClientBuffer(uint32_t device, const BufferHandle *buffer, int32_t fence)
{
    if (device || !buffer || !valid(*buffer)) return PARAM;
    if (wait_fence(fence)) return FAIL;
    int fd = fcntl(buffer->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return FAIL;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.client_fd >= 0) close(state.client_fd);
    state.client_fd = fd; state.client = *buffer; return OK;
}
API SetDisplayClientCrop(uint32_t device, const Rect *crop)
{ if (device || !crop || crop->x < 0 || crop->y < 0 || crop->w <= 0 || crop->h <= 0) return PARAM;
  std::lock_guard<std::mutex> lock(state.mutex); state.crop = *crop; return OK; }
API SetDisplayClientDamage(uint32_t device, const std::vector<Rect> *damage) { return device || !damage ? PARAM : OK; }
API ClearDisplayClientBuffer(uint32_t device)
{ if (device) return PARAM; std::lock_guard<std::mutex> lock(state.mutex);
  if (state.client_fd >= 0) close(state.client_fd); state.client_fd = -1; state.crop = {}; return OK; }
API Commit(uint32_t device, int32_t *fence)
{
    if (device || !fence) return PARAM;
    *fence = -1;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (initialize() || state.client_fd < 0 || wait_flip()) return FAIL;
    int slot = state.front == 0 ? 1 : 0;
    int result = copy_client(state.buffers[slot]);
    if (result) return result;
    if (state.front < 0) {
        if (drmModeSetCrtc(state.drm.fd, state.drm.crtc, state.buffers[slot].fb, 0, 0,
                           &state.drm.connector, 1, &state.drm.mode)) return FAIL;
        state.front = slot; return OK;
    }
    state.pending_slot = slot;
    if (drmModePageFlip(state.drm.fd, state.drm.crtc, state.buffers[slot].fb, DRM_MODE_PAGE_FLIP_EVENT, nullptr)) return FAIL;
    state.pending = true;
    return wait_flip(); // synchronous copy+flip: no outstanding client FD use
}
API GetDisplayReleaseFence(uint32_t device, std::vector<uint32_t> *ids, std::vector<int32_t> *fences)
{ if (device || !ids || !fences) return PARAM; ids->clear(); fences->clear(); return OK; }
API RegDisplayVBlankCallback(uint32_t device, Vblank callback, void *data)
{
    if (device) return PARAM;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (initialize()) return FAIL;
    state.vblank = callback; state.vblank_data = data;
    if (!state.ticker.joinable()) state.ticker = std::thread(ticker);
    state.wake.notify_all(); return OK;
}
API SetDisplayVsyncEnabled(uint32_t device, bool enabled)
{ if (device) return PARAM; std::lock_guard<std::mutex> lock(state.mutex);
  state.enabled = enabled; state.wake.notify_all(); return OK; }
API GetDisplayVBlankPeriod(uint32_t device, uint64_t *out)
{ if (device || !out) return PARAM; std::lock_guard<std::mutex> lock(state.mutex);
  if (initialize()) return FAIL; *out = period(); return OK; }

// These describe CLIENT layers. RenderService applies them during composition;
// this backend does not advertise any hardware layer or cursor capability.
#define LAYER_SETTER(name, type) \
API name(uint32_t device, uint32_t id, type) { \
    std::lock_guard<std::mutex> lock(state.mutex); return layer(device, id) ? OK : PARAM; }
LAYER_SETTER(SetLayerAlpha, const AuroraComposer::Alpha *)
LAYER_SETTER(SetLayerRegion, const Rect *)
LAYER_SETTER(SetLayerCrop, const Rect *)
LAYER_SETTER(SetLayerZorder, uint32_t)
LAYER_SETTER(SetLayerPreMulti, bool)
LAYER_SETTER(SetLayerTransformMode, int32_t)
LAYER_SETTER(SetLayerDirtyRegion, const std::vector<Rect> *)
LAYER_SETTER(SetLayerVisibleRegion, const std::vector<Rect> *)
LAYER_SETTER(SetLayerBlendType, int32_t)
LAYER_SETTER(SetLayerMaskInfo, int32_t)
LAYER_SETTER(SetLayerColor, const AuroraComposer::Color *)
API SetLayerBuffer(uint32_t device, uint32_t id, const BufferHandle *buffer, int32_t)
{ std::lock_guard<std::mutex> lock(state.mutex); return layer(device, id) && buffer ? OK : PARAM; }
API ClearLayerBuffer(uint32_t device, uint32_t id)
{ std::lock_guard<std::mutex> lock(state.mutex); return layer(device, id) ? OK : PARAM; }
// Optional extension entry points must exist for the vendor adapter loader.
// Unknown signatures are intentionally never inspected or claimed supported.
#define OPTIONAL(name) API name(...) { return UNSUPPORTED; }
OPTIONAL(CreateVirtualDisplay) OPTIONAL(DestroyVirtualDisplay) OPTIONAL(SetVirtualDisplayBuffer)
OPTIONAL(RegSeamlessChangeCallback) OPTIONAL(GetDisplaySupportedModesExt) OPTIONAL(SetDisplayModeAsync)
OPTIONAL(SetLayerPerFrameParameter) OPTIONAL(SetDisplayPerFrameParameter) OPTIONAL(GetSupportedLayerPerFrameParameterKey)
OPTIONAL(RegRefreshCallback) OPTIONAL(GetHDRCapabilityInfos) OPTIONAL(RegDisplayVBlankIdleCallback)
OPTIONAL(EnableHardwareCursorStats) OPTIONAL(GetHardwareCursorStats) OPTIONAL(FastPresent)
OPTIONAL(GetDisplayIdentificationData) OPTIONAL(RegHwcEventCallback) OPTIONAL(GetDumpInfo)
OPTIONAL(UpdateHardwareCursor) OPTIONAL(SetDisplayConstraint) OPTIONAL(SetDisplayActiveRegion)
OPTIONAL(SetDisplayOverlayResolution) OPTIONAL(UpdateConfig)
OPTIONAL(GetSupportLayerType) OPTIONAL(CommitTunnelLayer) OPTIONAL(SetTunnelLayerBuffer)
OPTIONAL(SetTunnelLayerId) OPTIONAL(SetTunnelLayerPosition) OPTIONAL(SetTunnelLayerProperty)
API ClearClientBuffer(uint32_t device) { return ClearDisplayClientBuffer(device); }
API InitDisplay() { std::lock_guard<std::mutex> lock(state.mutex); return initialize(); }
// Full teardown occurs at library unload, after the service releases callbacks.
API DeinitDisplay() { return UNSUPPORTED; }
