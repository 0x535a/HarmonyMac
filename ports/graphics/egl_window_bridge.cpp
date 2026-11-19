/* SPDX-License-Identifier: GPL-3.0-only
 * OHOS NativeWindow WSI over Mesa GBM.
 * A private GBM window supplies default-framebuffer semantics; each swap copies
 * pixels into the NativeWindow queue. GL rendering is delegated to Mesa;
 * bounded clear tracing observes the stock transparent composition pass.
 */
#include "include/aurora_drm.h"
#include "include/buffer_handle.h"
#include "include/display_type.h"
#include "include/frame_diagnostics.h"
#include "include/frame_samples.h"
#include "include/linear_yuv420.h"
#include "include/perf_trace.h"
#include "include/pixel_copy.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unwind.h>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr EGLenum PLATFORM_OHOS = 0x34e0, NATIVE_BUFFER_OHOS = 0x34e1;
struct NativeWindow;
struct NativeBuffer;
struct Region { struct Rect { int32_t x, y; uint32_t w, h; } *rects; int32_t count; };
struct NativeApi {
    int (*request)(NativeWindow *, NativeBuffer **, int *);
    int (*flush)(NativeWindow *, NativeBuffer *, int, Region);
    int (*abort)(NativeWindow *, NativeBuffer *);
    int (*option)(NativeWindow *, int, ...);
    BufferHandle *(*handle)(NativeBuffer *);
    int (*reference)(void *);
    int (*unreference)(void *);
};
void *mesa()
{
    static void *library = dlopen("/system/lib64/aurora-mesa/libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    return library;
}
template<typename T> T real(const char *name)
{
    if (!mesa()) return nullptr;
    void *symbol = dlsym(mesa(), name);
    if (!symbol && strcmp(name, "eglGetProcAddress")) {
        auto proc = reinterpret_cast<decltype(&eglGetProcAddress)>(dlsym(mesa(), "eglGetProcAddress"));
        if (proc) symbol = reinterpret_cast<void *>(proc(name));
    }
    return reinterpret_cast<T>(symbol);
}
NativeApi *native_api()
{
    static NativeApi api = [] {
        NativeApi value = {};
        // RTLD_DEFAULT enables a standalone fixture provider; production falls
        // back to the unchanged surface library's public NativeWindow exports.
        void *library = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
        auto lookup = [library](const char *name) {
            void *symbol = dlsym(RTLD_DEFAULT, name);
            return symbol ? symbol : (library ? dlsym(library, name) : nullptr);
        };
#define NATIVE(field, name) value.field = reinterpret_cast<decltype(value.field)>(lookup(name))
        NATIVE(request, "OH_NativeWindow_NativeWindowRequestBuffer");
        NATIVE(flush, "OH_NativeWindow_NativeWindowFlushBuffer");
        NATIVE(abort, "OH_NativeWindow_NativeWindowAbortBuffer");
        NATIVE(option, "OH_NativeWindow_NativeWindowHandleOpt");
        NATIVE(handle, "OH_NativeWindow_GetBufferHandleFromNative");
        NATIVE(reference, "OH_NativeWindow_NativeObjectReference");
        NATIVE(unreference, "OH_NativeWindow_NativeObjectUnreference");
#undef NATIVE
        return value;
    }();
    return api.request && api.flush && api.abort && api.option && api.handle && api.reference && api.unreference ? &api : nullptr;
}
thread_local EGLint local_error = EGL_SUCCESS;
EGLBoolean error(EGLint code) { local_error = code; return EGL_FALSE; }
struct Display {
    gbm_device *device = nullptr;
    EGLDisplay egl = EGL_NO_DISPLAY;
    bool asahi = false;
    bool renderer_reported = false;
};
std::mutex mutex;
Display display;

void record_graphics_diagnostic(const char *name, const char *value)
{
    const char *directory = getenv("AURORA_RENDER_DIAGNOSTICS_DIR");
    if (!directory || !*directory) return;
    const int saved_errno = errno;
    char path[256];
    const int length = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length > 0 && size_t(length) < sizeof(path)) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            dprintf(fd, "%s\n", value);
            close(fd);
        }
    }
    errno = saved_errno;
}

int open_asahi_render_node()
{
    for (int index = 128; index < 192; ++index) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/dri/renderD%d", index);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        auto version = drmGetVersion(fd);
        bool asahi = version && version->name && !strcmp(version->name, "asahi");
        if (version) drmFreeVersion(version);
        if (asahi) {
            std::fprintf(stderr, "AURORA_EGL_ASAHI_DRM_NODE=%s\n", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

struct FrontSlot {
    gbm_bo *bo = nullptr;
    NativeBuffer *buffer = nullptr;
    struct stat identity {};
    bool locked = false;
};

// GPU blit targets keep their dma-buf import across frames. Re-importing the
// same 24 MB NativeWindow buffer every swap made Mesa bind and unbind a GPU VA
// per frame, which the M3 kernel retires lazily.
struct BlitImage {
    dev_t dev = 0;
    ino_t ino = 0;
    int32_t width = 0, height = 0, stride = 0, format = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    unsigned long long used = 0;
};
// Read-only diagnostic mappings of published source frames.
struct SourceMap {
    dev_t dev = 0;
    ino_t ino = 0;
    void *map = MAP_FAILED;
    size_t size = 0;
};
struct Window {
    EGLDisplay display;
    EGLConfig config;
    NativeWindow *native;
    EGLSurface backing = EGL_NO_SURFACE;
    gbm_surface *gbm = nullptr;
    int width = 0, height = 0;
    std::vector<EGLint> creation_attributes;
    bool destroyed = false;
    unsigned swaps = 0;
    AuroraFrameCapture capture;
    std::vector<unsigned char> readback;
    bool fresh_backing = true;
    bool age_aware = false, back_cleared = true;
    unsigned published_in_pool = 0;
    unsigned trace_pool = 0;
    EGLint trace_age = -1;
    bool scanout_format = false;
    bool front_disabled = false;
    bool front_recover_backing = false;
    bool scanout_backing = false;
    std::vector<FrontSlot> fronts;
    std::vector<BlitImage> blit_images;
    unsigned long long blit_uses = 0;
    std::vector<SourceMap> source_maps;
    uint32_t marker_serial = 0;
    bool marked_frame = false;
    EGLint declared_damage[4] = {0, 0, 0, 0};
    EGLint declared_count = -1;
    void release_blit_images() {
        auto destroy = real<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
        for (auto &entry : blit_images)
            if (destroy && entry.image != EGL_NO_IMAGE_KHR) destroy(display, entry.image);
        blit_images.clear();
    }
    void invalidate_history() {
        published_in_pool = 0;
        back_cleared = true;
        trace_age = -1;
    }
    void clear_fronts(gbm_surface *surface) {
        for (auto &slot : fronts) {
            if (slot.locked) gbm_surface_release_buffer(surface, slot.bo);
            native_api()->unreference(slot.buffer);
        }
        fronts.clear();
    }
    ~Window() {
        // No renderer can use this generation again. Drop GBM locks while
        // Mesa's surface-private storage is still alive. Native queue and
        // KMS references independently pin any last visible DMA-BUF.
        clear_fronts(gbm);
        release_blit_images();
        for (auto &entry : source_maps) if (entry.map != MAP_FAILED) munmap(entry.map, entry.size);
        if (backing != EGL_NO_SURFACE) {
            auto destroy = real<decltype(&eglDestroySurface)>("eglDestroySurface");
            if (destroy) destroy(display, backing);
        }
        if (gbm) gbm_surface_destroy(gbm);
        if (native) native_api()->unreference(native);
    }
};
std::map<EGLSurface, std::shared_ptr<Window>> windows;
// Destroyed virtual handles can still be current on other render threads.
// eglTerminate must retire their GBM locks before Mesa tears down the display.
std::vector<std::weak_ptr<Window>> deferred_windows;
std::map<EGLDisplay, std::string> extension_strings;
thread_local std::shared_ptr<Window> current_draw, current_read;
thread_local unsigned clear_calls = 0;
constexpr unsigned image_trace_limit = 64;
std::atomic<unsigned> import_failures{0};
thread_local unsigned thread_import_failure = 0;

void trace_image_failure(const char *route, int fd, int width, int height, int format,
                         int stride, int offset, int size, uint64_t usage)
{
    if (!aurora_display_trace_enabled()) return;
    const unsigned id = import_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id > image_trace_limit) return;
    const int saved_errno = errno;
    const auto now = aurora_display_time_ns();
    struct stat identity{};
    const bool identified = fd >= 0 && !fstat(fd, &identity);
    thread_import_failure = id;
    aurora_display_trace("AURORA_EGL_IMAGE_IMPORT_FAIL id=%u mono_ns=%llu tid=%ld route=%s fd=%d identified=%d"
        " device=%llu inode=%llu size=%dx%d format=0x%x stride=%d offset=%d bytes=%d usage=0x%llx",
        id, now, syscall(SYS_gettid), route, fd, identified,
        static_cast<unsigned long long>(identity.st_dev), static_cast<unsigned long long>(identity.st_ino),
        width, height, static_cast<unsigned>(format), stride, offset, size, static_cast<unsigned long long>(usage));
    errno = saved_errno;
}

template<class Attr>
void trace_dma_buf_failure(const Attr *attributes)
{
    if (!aurora_display_trace_enabled() || !attributes) return;
    int fd = -1, width = -1, height = -1, format = 0, stride = -1, offset = -1;
    for (unsigned n = 0; n < 64 && attributes[n * 2] != EGL_NONE; ++n) {
        const int value = static_cast<int>(attributes[n * 2 + 1]);
        switch (attributes[n * 2]) {
        case EGL_WIDTH: width = value; break;
        case EGL_HEIGHT: height = value; break;
        case EGL_LINUX_DRM_FOURCC_EXT: format = value; break;
        case EGL_DMA_BUF_PLANE0_FD_EXT: fd = value; break;
        case EGL_DMA_BUF_PLANE0_OFFSET_EXT: offset = value; break;
        case EGL_DMA_BUF_PLANE0_PITCH_EXT: stride = value; break;
        }
    }
    trace_image_failure("mesa_dmabuf", fd, width, height, format, stride, offset, 0, 0);
}

void trace_queue_origin(const Window &window)
{
    static const bool enabled = [] {
        const char *value = std::getenv("AURORA_EGL_QUEUE_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    static std::atomic<unsigned> budget{0};
    if (!enabled || (window.swaps != 0 && window.swaps != 4) || budget++ >= 24) return;
    int format = -1;
    uint64_t usage = 0;
    EGLint visual = 0;
    native_api()->option(window.native, 2, &format);
    native_api()->option(window.native, 4, &usage);
    real<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib")(
        window.display, window.config, EGL_NATIVE_VISUAL_ID, &visual);
    std::fprintf(stderr, "AURORA_QUEUE_TRACE window=%p frame=%u size=%dx%d format=%d usage=0x%llx visual=0x%x\n",
        &window, window.swaps, window.width, window.height, format,
        static_cast<unsigned long long>(usage), visual);
    struct Trace { const void *window; unsigned depth; } trace{&window, 0};
    _Unwind_Backtrace([](_Unwind_Context *context, void *state) {
        auto &trace = *static_cast<Trace *>(state);
        if (++trace.depth > 24) return _URC_END_OF_STACK;
        const auto pc = _Unwind_GetIP(context);
        Dl_info info{};
        if (pc && dladdr(reinterpret_cast<const void *>(pc), &info) && info.dli_fbase && info.dli_fname) {
            const char *base = std::strrchr(info.dli_fname, '/');
            std::fprintf(stderr, "AURORA_QUEUE_STACK window=%p at=%s+0x%llx\n", trace.window,
                base ? base + 1 : info.dli_fname,
                static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        }
        return _URC_NO_REASON;
    }, &trace);
}

bool is_stock_scanout_window(NativeWindow *window)
{
    const char *enabled = std::getenv("AURORA_EGL_SCANOUT_FORMAT");
    if (!enabled || std::strcmp(enabled, "1")) return false;
    using GetId = int (*)(NativeWindow *, uint64_t *);
    using GetName = std::string (*)(void *);
    static const auto get_surface = [] {
        void *library = dlopen("/system/lib64/chipset-sdk-sp/libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
        struct Symbols { GetId id; GetName name; const void *producer_id; };
        if (!library) return Symbols{};
        auto id = reinterpret_cast<GetId>(dlsym(library, "OH_NativeWindow_GetSurfaceId"));
        constexpr uint32_t access_signature[] = {
            0xaa0003e8, 0xf9400c00, 0xb4000460, 0xb9401108, 0x5289c889, 0x72aae929, 0x6b09011f,
        };
        // Match the installed AArch64 getter before reading the private
        // pointer it validates at NativeWindow + 0x18. Other builds keep the
        // normal format, even if their public function names match.
        if (!id || std::memcmp(reinterpret_cast<const char *>(id) + 0x20,
                                access_signature, sizeof(access_signature))) return Symbols{};
        return Symbols{id, reinterpret_cast<GetName>(dlsym(library, "_ZN4OHOS15ProducerSurface7GetNameEv")),
                       dlsym(library, "_ZNK4OHOS15ProducerSurface11GetUniqueIdEv")};
    }();
    static std::atomic<unsigned> failures{0};
    auto reject = [](const char *reason) {
        if (failures++ < 8) std::fprintf(stderr, "AURORA_FRAMEBUFFER_FORMAT skip=%s\n", reason);
        return false;
    };
    uint64_t id = 0;
    if (!get_surface.id || !get_surface.name || !get_surface.producer_id)
        return reject("surface-abi");
    if (get_surface.id(window, &id) || !id) return reject("surface-id");
    void *surface = nullptr;
    std::memcpy(&surface, reinterpret_cast<const char *>(window) + 0x18, sizeof(surface));
    if (!surface) return reject("surface-null");
    const void *vptr = nullptr;
    std::memcpy(&vptr, surface, sizeof(vptr));
    // The same getter dispatches Surface::GetUniqueId via slot 0x108.
    // Match that concrete method instead of assuming the vtable address point.
    if (!vptr) return reject("surface-vtable");
    const void *get_id = nullptr;
    std::memcpy(&get_id, static_cast<const char *>(vptr) + 0x108, sizeof(get_id));
    if (get_id != get_surface.producer_id) return reject("surface-class");
    const std::string name = get_surface.name(surface);
    if (name != "FrameBuffer") {
        if (failures++ < 8)
            std::fprintf(stderr, "AURORA_FRAMEBUFFER_FORMAT skip=window-name name=%.*s\n",
                         96, name.c_str());
        return false;
    }
    return true;
}

void configure_scanout_format(Window &window)
{
    if (window.scanout_format) {
        // NativeWindowOperation::SET_FORMAT from the matched public header.
        constexpr int SET_FORMAT = 3;
        native_api()->option(window.native, SET_FORMAT, static_cast<int>(PIXEL_FMT_BGRA_8888));
    }
}
std::shared_ptr<Window> find(EGLSurface surface)
{
    auto it = windows.find(surface);
    return it == windows.end() ? nullptr : it->second;
}
EGLSurface backing(EGLSurface surface) { auto window = find(surface); return window ? window->backing : surface; }
bool geometry(NativeWindow *window, int &width, int &height)
{
    // Public OHOS GET_BUFFER_GEOMETRY takes height first, then width.
    return !native_api()->option(window, 1, &height, &width) && width > 0 && height > 0 &&
           width <= 16384 && height <= 16384 && uint64_t(width) * height <= 67108864;
}

bool front_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("AURORA_EGL_GBM_FRONT");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

bool framebuffer_usage(NativeWindow *window, uint64_t &usage)
{
    return !native_api()->option(window, 4, &usage) && (usage & HBM_USE_MEM_FB) &&
           !(usage & HBM_USE_PROTECTED);
}

bool create_backing(Window &window, int width, int height, const EGLint *attributes)
{
    auto get = real<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib");
    EGLint format;
    if (!get || !get(window.display, window.config, EGL_NATIVE_VISUAL_ID, &format)) return false;
    uint32_t usage = GBM_BO_USE_RENDERING;
    if (!display.asahi) usage |= GBM_BO_USE_LINEAR;
    uint64_t native_usage = 0;
    const bool scanout = front_enabled() && !window.front_disabled &&
        (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) &&
        framebuffer_usage(window.native, native_usage);
    auto create = real<decltype(&eglCreateWindowSurface)>("eglCreateWindowSurface");
    for (unsigned attempt = scanout ? 0 : 1; attempt < 2; ++attempt) {
        const uint32_t flags = attempt == 0 ? usage | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT : usage;
        window.gbm = gbm_surface_create(display.device, width, height, format, flags);
        if (!window.gbm) continue;
        window.backing = create(window.display, window.config,
            reinterpret_cast<EGLNativeWindowType>(window.gbm), attributes);
        if (window.backing != EGL_NO_SURFACE) {
            window.scanout_backing = (attempt == 0);
            break;
        }
        gbm_surface_destroy(window.gbm);
        window.gbm = nullptr;
        if (attempt == 0) real<decltype(&eglGetError)>("eglGetError")();
    }
    if (!window.gbm) return false;
    window.width = width; window.height = height;
    window.fresh_backing = true;
    window.back_cleared = true;
    window.published_in_pool = 0;
    window.trace_age = -1;
    if (aurora_display_trace_enabled()) {
        static unsigned generations = 0;  // All backing changes hold the WSI mutex.
        const unsigned previous = window.trace_pool;
        window.trace_pool = ++generations;
        if (generations <= 128)
            aurora_display_trace("AURORA_EGL_POOL_CREATE mono_ns=%llu window=%p pool=%u previous_pool=%u size=%dx%d scanout=%d",
                aurora_display_time_ns(), static_cast<void *>(&window), window.trace_pool, previous,
                width, height, window.scanout_backing);
    }
    return true;
}
bool resize(Window &window, int width, int height)
{
    if (window.width == width && window.height == height && !window.front_recover_backing) return true;
    auto old_egl = window.backing; auto old_gbm = window.gbm;
    int old_width = window.width, old_height = window.height;
    const bool old_scanout = window.scanout_backing, old_fresh = window.fresh_backing;
    const bool old_cleared = window.back_cleared;
    const unsigned old_published = window.published_in_pool;
    const unsigned old_pool = window.trace_pool;
    const EGLint old_age = window.trace_age;
    window.gbm = nullptr;
    if (!create_backing(window, width, height, window.creation_attributes.empty() ? nullptr : window.creation_attributes.data())) {
        window.backing = old_egl; window.gbm = old_gbm; return false;
    }
    auto make = real<decltype(&eglMakeCurrent)>("eglMakeCurrent");
    auto context = real<decltype(&eglGetCurrentContext)>("eglGetCurrentContext")();
    EGLSurface draw = current_draw ? current_draw->backing : real<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface")(EGL_DRAW);
    EGLSurface read = current_read ? current_read->backing : real<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface")(EGL_READ);
    if (!make(window.display, draw, read, context)) {
        real<decltype(&eglDestroySurface)>("eglDestroySurface")(window.display, window.backing);
        gbm_surface_destroy(window.gbm); window.backing = old_egl; window.gbm = old_gbm;
        window.width = old_width; window.height = old_height;
        window.scanout_backing = old_scanout; window.fresh_backing = old_fresh;
        window.back_cleared = old_cleared; window.published_in_pool = old_published;
        window.trace_pool = old_pool; window.trace_age = old_age;
        return false;
    }
    window.clear_fronts(old_gbm);
    real<decltype(&eglDestroySurface)>("eglDestroySurface")(window.display, old_egl);
    gbm_surface_destroy(old_gbm);
    window.front_recover_backing = false;
    return true;
}
bool wait_fence(int fd)
{
    if (fd < 0) return true;
    pollfd item = {fd, POLLIN, 0};
    int ret;
    do { ret = poll(&item, 1, 3000); } while (ret < 0 && errno == EINTR);
    close(fd);
    return ret > 0 && (item.revents & POLLIN) && !(item.revents & (POLLERR | POLLNVAL));
}
bool valid(const BufferHandle *buffer)
{
    return buffer && buffer->fd >= 0 && buffer->width > 0 && buffer->height > 0 &&
        buffer->width <= 16384 && buffer->height <= 16384 && buffer->size > 0 &&
        buffer->stride >= int64_t(buffer->width) * 4 && int64_t(buffer->stride) * buffer->height <= buffer->size &&
        !(buffer->usage & HBM_USE_PROTECTED) &&
        (buffer->format == PIXEL_FMT_RGBA_8888 || buffer->format == PIXEL_FMT_RGBX_8888 ||
         buffer->format == PIXEL_FMT_BGRA_8888 || buffer->format == PIXEL_FMT_BGRX_8888);
}

struct StockFrontApi {
    struct Config { int32_t width, height, format, usage; };
    void *(*allocate)(const Config *) = nullptr;
    int (*release)(void *) = nullptr;
    void (*set_handle)(void *, BufferHandle *) = nullptr;
    NativeBuffer *(*wrap)(void *) = nullptr;
    int (*attach)(NativeWindow *, NativeBuffer *) = nullptr;
    int (*detach)(NativeWindow *, NativeBuffer *) = nullptr;
    BufferHandle *(*allocate_handle)(uint32_t, uint32_t) = nullptr;

    explicit operator bool() const {
        return allocate && release && set_handle && wrap && attach && detach && allocate_handle;
    }
};

const StockFrontApi &stock_front_api()
{
    static const auto api = [] {
        StockFrontApi out;
        void *library = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return out;
#define FRONT_SYMBOL(field, name) out.field = reinterpret_cast<decltype(out.field)>(dlsym(library, name))
        FRONT_SYMBOL(allocate, "OH_NativeBuffer_Alloc");
        FRONT_SYMBOL(release, "OH_NativeBuffer_Unreference");
        FRONT_SYMBOL(set_handle, "_ZN4OHOS17SurfaceBufferImpl15SetBufferHandleEP12BufferHandle");
        FRONT_SYMBOL(wrap, "OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer");
        FRONT_SYMBOL(attach, "OH_NativeWindow_NativeWindowAttachBuffer");
        FRONT_SYMBOL(detach, "OH_NativeWindow_NativeWindowDetachBuffer");
        FRONT_SYMBOL(allocate_handle, "AllocateBufferHandle");
#undef FRONT_SYMBOL
        return out;
    }();
    return api;
}

struct FrontReclaim {
    unsigned long long device = 0, inode = 0, time_ns = 0;
};

bool reclaim_front(Window &window, FrontReclaim *record)
{
    auto api = native_api();
    int timeout = 0;
    if (api->option(window.native, 11, &timeout) || api->option(window.native, 10, 0)) return false;
    // A copy-path buffer can be at the front of the same free list. Keep it
    // requested until the search finishes so CancelBuffer cannot put it back
    // ahead of every retired GBM buffer.
    NativeBuffer *skipped[4] = {};
    unsigned skipped_count = 0;
    bool reclaimed = false;
    while (skipped_count < sizeof(skipped) / sizeof(skipped[0]) && !reclaimed) {
        NativeBuffer *released = nullptr;
        int fence = -1;
        if (api->request(window.native, &released, &fence) || !released) {
            if (fence >= 0) close(fence);
            break;
        }
        auto handle = api->handle(released);
        struct stat st {};
        if (handle && !fstat(handle->fd, &st)) {
            for (auto &slot : window.fronts) {
                if (!slot.locked || slot.identity.st_dev != st.st_dev || slot.identity.st_ino != st.st_ino)
                    continue;
                bool ready = wait_fence(fence);
                fence = -1;
                if (ready && !stock_front_api().detach(window.native, released)) {
                    gbm_surface_release_buffer(window.gbm, slot.bo);
                    slot.locked = false;
                    if (record)
                        *record = {static_cast<unsigned long long>(slot.identity.st_dev),
                                   static_cast<unsigned long long>(slot.identity.st_ino), aurora_display_time_ns()};
                    reclaimed = true;
                }
                break;
            }
        }
        if (fence >= 0) close(fence);
        if (!reclaimed) skipped[skipped_count++] = released;
    }
    for (unsigned i = 0; i < skipped_count; ++i) api->abort(window.native, skipped[i]);
    api->option(window.native, 10, timeout);
    return reclaimed;
}

FrontSlot *wrap_front(Window &window, gbm_bo *bo, int format, uint64_t usage)
{
    const unsigned stride = gbm_bo_get_stride(bo);
    if (stride < uint64_t(window.width) * 4 || stride > INT32_MAX) return nullptr;
    const int fd = gbm_bo_get_fd(bo);
    if (fd < 0) return nullptr;
    struct stat identity {};
    const off_t length = lseek(fd, 0, SEEK_END);
    if (length < int64_t(stride) * window.height || length > INT32_MAX || fstat(fd, &identity)) {
        close(fd);
        return nullptr;
    }
    // Mesa can destroy an unused BO and allocate a new one at the same
    // address. A wrapper is valid only for the same exported storage.
    FrontSlot *unused = nullptr;
    for (auto &slot : window.fronts) {
        if (slot.bo == bo && slot.identity.st_dev == identity.st_dev && slot.identity.st_ino == identity.st_ino) {
            close(fd);
            return slot.locked ? nullptr : &slot;
        }
        if (!slot.locked) unused = &slot;
    }
    if (window.fronts.size() >= 4 && !unused) { close(fd); return nullptr; }
    const auto &api = stock_front_api();
    const StockFrontApi::Config config{window.width, window.height, format, static_cast<int32_t>(usage)};
    void *stock = api.allocate(&config);
    if (!stock) { close(fd); return nullptr; }
    BufferHandle *handle = api.allocate_handle(0, 0);
    if (!handle) { api.release(stock); close(fd); return nullptr; }
    handle->fd = fd;
    handle->width = window.width;
    handle->height = window.height;
    handle->stride = static_cast<int32_t>(stride);
    handle->size = static_cast<int32_t>(length);
    handle->format = format;
    handle->usage = usage;
    api.set_handle(stock, handle);
    NativeBuffer *buffer = api.wrap(stock);
    api.release(stock);
    if (!buffer) return nullptr;
    if (window.fronts.size() >= 4) {
        native_api()->unreference(unused->buffer);
        *unused = {bo, buffer, identity, false};
        return unused;
    }
    window.fronts.push_back({bo, buffer, identity, false});
    return &window.fronts.back();
}

int front_render_fence(EGLDisplay egl)
{
    auto create = real<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
    auto duplicate = real<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");
    auto destroy = real<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR");
    auto query = real<decltype(&eglQueryString)>("eglQueryString");
    const char *extensions = query ? query(egl, EGL_EXTENSIONS) : nullptr;
    if (extensions && std::strstr(extensions, "EGL_ANDROID_native_fence_sync") && create && duplicate && destroy) {
        EGLSyncKHR sync = create(egl, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush();
            int fence = duplicate(egl, sync);
            destroy(egl, sync);
            if (fence >= 0) return fence;
        }
    }
    glFinish();
    return -1;
}

void reset_destroyed_backbuffer(Window &window);

struct SwapDamage {
    const EGLint *rects;
    EGLint count;
    const char *extension;

    EGLBoolean swap(Window &window, decltype(&eglSwapBuffers) plain) const
    {
        if (extension && count && window.published_in_pool) {
            auto fn = real<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(extension);
            if (fn) return fn(window.display, window.backing, rects, count);
        }
        return plain(window.display, window.backing);
    }

    // EGL uses bottom-left coordinates. OHOS NativeWindow uses top-left.
    Region native(const Window &w, Region::Rect &bounds) const
    {
        if (!count || !w.published_in_pool || count > 256) return {nullptr, 0};
        int x0 = w.width, y0 = w.height, x1 = 0, y1 = 0;
        for (EGLint i = 0; i < count; ++i) {
            const EGLint *r = rects + 4*i;
            const int left = std::clamp(r[0], 0, w.width);
            const int bottom = std::clamp(r[1], 0, w.height);
            const int right = std::clamp<int64_t>(int64_t(r[0]) + r[2], 0, w.width);
            const int top = std::clamp<int64_t>(int64_t(r[1]) + r[3], 0, w.height);
            if (right <= left || top <= bottom) continue;
            x0 = std::min(x0, left); x1 = std::max(x1, right);
            y0 = std::min(y0, w.height-top); y1 = std::max(y1, w.height-bottom);
        }
        // The stock HdiFramebufferSurface uses the one-Rect AcquireBuffer
        // entry point. A multi-rect NativeWindow flush can enqueue successfully
        // and subsequently fail acquisition. The union is conservative.
        if (x1 <= x0 || y1 <= y0) return {nullptr, 0};
        bounds = {x0, y0, uint32_t(x1-x0), uint32_t(y1-y0)};
        return {&bounds, 1};
    }
};

bool valid_damage(const EGLint *rects, EGLint count)
{
    if (count < 0 || (count && !rects)) return false;
    for (EGLint i = 0; i < count; ++i)
        if (rects[size_t(i)*4+2] < 0 || rects[size_t(i)*4+3] < 0) return false;
    return true;
}

void did_publish(Window &window)
{
    if (window.published_in_pool < UINT32_MAX) ++window.published_in_pool;
    window.back_cleared = false;
    window.trace_age = -1;
    reset_destroyed_backbuffer(window);
}

std::optional<EGLBoolean> try_front_swap(Window &window, decltype(&eglSwapBuffers) swap, AuroraPerfTrace &trace,
                                         const SwapDamage &damage)
{
    unsigned locked = 0;
    for (const auto &slot : window.fronts) locked += slot.locked;
    if (!window.scanout_backing || !front_enabled() || window.front_disabled || !stock_front_api() || AuroraFrameCapture::token()) {
        if (locked) window.front_recover_backing = true;
        return std::nullopt;
    }
    uint64_t usage = 0;
    int format = -1, width = 0, height = 0;
    EGLint visual = 0;
    auto get = real<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib");
    if (!framebuffer_usage(window.native, usage) || usage > INT32_MAX ||
        native_api()->option(window.native, 2, &format) ||
        !get(window.display, window.config, EGL_NATIVE_VISUAL_ID, &visual) ||
        !((format == PIXEL_FMT_BGRA_8888 && visual == DRM_FORMAT_ARGB8888) ||
          (format == PIXEL_FMT_BGRX_8888 && visual == DRM_FORMAT_XRGB8888)) ||
        !geometry(window.native, width, height) || width != window.width || height != window.height) {
        if (locked) window.front_recover_backing = true;
        return std::nullopt;
    }

    static unsigned trace_sequence = 0; // protected by the WSI mutex
    const bool log_frame = aurora_display_trace_enabled() && trace_sequence < 16384;
    FrontReclaim reclaimed;
    if (locked >= 2 && reclaim_front(window, log_frame ? &reclaimed : nullptr)) --locked;
    if (locked >= 3 || !gbm_surface_has_free_buffers(window.gbm)) {
        window.front_disabled = true;
        window.front_recover_backing = true;
        std::fputs("AURORA_GBM_FRONT_FALLBACK no retired buffer\n", stderr);
        return std::nullopt;
    }

    if (!damage.swap(window, swap)) {
        // Mesa can advance its BO ages before a swap ultimately fails.
        window.invalidate_history();
        return EGL_FALSE;
    }
    trace.mark("gbm_mesa_swap");
    auto dropped = [&](const char *reason, EGLint code = EGL_BAD_ALLOC) {
        window.front_disabled = true;
        window.front_recover_backing = true;
        std::fprintf(stderr, "AURORA_GBM_FRONT_FALLBACK %s after Mesa swap\n", reason);
        window.invalidate_history();
        // The backing already swapped but OHOS did not receive a frame.
        // Bind a fresh generation before the caller begins its next draw.
        if (resize(window, window.width, window.height)) reset_destroyed_backbuffer(window);
        return error(code);
    };

    gbm_bo *bo = gbm_surface_lock_front_buffer(window.gbm);
    if (!bo) return dropped("lock front");
    if (gbm_bo_get_plane_count(bo) != 1 || gbm_bo_get_offset(bo, 0) != 0 ||
        gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_LINEAR || gbm_bo_get_format(bo) != static_cast<uint32_t>(visual)) {
        gbm_surface_release_buffer(window.gbm, bo);
        return dropped("incompatible layout");
    }
    FrontSlot *slot = wrap_front(window, bo, format, usage);
    if (!slot || stock_front_api().attach(window.native, slot->buffer)) {
        gbm_surface_release_buffer(window.gbm, bo);
        return dropped("attach stock queue");
    }
    slot->locked = true;
    // Mesa may flush or resolve the drawable during SwapBuffers. Fence the
    // locked, post-swap image before publishing it to the stock queue.
    const int render_complete = front_render_fence(window.display);
    trace.mark("gbm_render_fence");
    Region::Rect rects{};
    const Region region = damage.native(window, rects);
    const auto submitted = log_frame ? aurora_display_time_ns() : 0;
    const auto last_failure = log_frame ? std::min(import_failures.load(std::memory_order_relaxed), image_trace_limit) : 0;
    if (native_api()->flush(window.native, slot->buffer, render_complete, region)) {
        // Flush consumed the fence. Detach a still-requested attachment;
        // never hand an ambiguously owned BO back for drawing.
        if (!stock_front_api().detach(window.native, slot->buffer)) {
            gbm_surface_release_buffer(window.gbm, bo);
            slot->locked = false;
        } else {
            native_api()->abort(window.native, slot->buffer);
        }
        return dropped("flush", EGL_BAD_NATIVE_WINDOW);
    }
    ++window.swaps;
    if (window.swaps <= 3 || window.swaps % 120 == 0) {
        std::fprintf(stderr, "AURORA_GBM_FRONT_SUBMITTED frame=%u size=%dx%d sync=%s\n", window.swaps,
            width, height, render_complete >= 0 ? "sync_file" : "glFinish");
    }
    if (log_frame) {
        aurora_display_trace("AURORA_DIRECT_BUFFER_SUBMIT device=%llu inode=%llu frame=%u serial=%u submit_ns=%llu"
            " tid=%ld window=%p pool=%u pool_frame=%u age=%d damage_rects=%d queue_damage_rects=%d"
            " queue_rect=%d,%d,%u,%u sync_file=%d last_import_failure=%u thread_import_failure=%u"
            " reclaimed_device=%llu reclaimed_inode=%llu reclaimed_ns=%llu",
            static_cast<unsigned long long>(slot->identity.st_dev),
            static_cast<unsigned long long>(slot->identity.st_ino), window.swaps, ++trace_sequence,
            submitted, syscall(SYS_gettid), static_cast<void *>(&window), window.trace_pool, window.published_in_pool + 1,
            window.trace_age, damage.count, region.count, rects.x, rects.y, rects.w, rects.h, render_complete >= 0,
            last_failure, thread_import_failure, reclaimed.device, reclaimed.inode, reclaimed.time_ns);
    }
    trace.mark("gbm_front_native_flush");
    trace.result(0);
    did_publish(window);
    return EGL_TRUE;
}
bool queue_geometry(NativeWindow *native, int &width, int &height)
{
    if (geometry(native,width,height)) return true;
    // A stock RS window may have no producer geometry yet. The consumer's
    // default size is authoritative; obtain it from a real requested buffer.
    // Never invent panel dimensions or leave this discovery buffer acquired.
    auto api=native_api();NativeBuffer *buffer=nullptr;int fence=-1;
    if (!api || api->request(native,&buffer,&fence)) {
        if (fence>=0) close(fence);
        return false;
    }
    bool waited=wait_fence(fence);
    auto handle=buffer ? api->handle(buffer) : nullptr;
    bool ok=waited && valid(handle);
    if (ok) {width=handle->width;height=handle->height;}
    int aborted=buffer ? api->abort(native,buffer) : -1;
    return ok && !aborted;
}

EGLImageKHR import_linear_buffer(EGLDisplay egl, const BufferHandle &handle, const char *route)
{
    auto create = real<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    if (!create) return EGL_NO_IMAGE_KHR;
    auto create_image = [&](const EGLint *attrs) {
        const EGLImageKHR image = create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (image == EGL_NO_IMAGE_KHR)
            trace_image_failure(route, handle.fd, handle.width, handle.height, handle.format,
                handle.stride, 0, handle.size, handle.usage);
        return image;
    };
    if (AuroraIsLinearYuv420(handle.format)) {
        if (!AuroraValidLinearYuv420(handle)) return EGL_NO_IMAGE_KHR;
        const EGLint format = handle.format == PIXEL_FMT_YCBCR_420_SP ? DRM_FORMAT_NV12 : DRM_FORMAT_NV21;
        const EGLint attrs[] = {
            EGL_WIDTH, handle.width, EGL_HEIGHT, handle.height, EGL_LINUX_DRM_FOURCC_EXT, format,
            EGL_DMA_BUF_PLANE0_FD_EXT, handle.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, handle.stride,
            EGL_DMA_BUF_PLANE1_FD_EXT, handle.fd, EGL_DMA_BUF_PLANE1_OFFSET_EXT, handle.stride * handle.height,
            EGL_DMA_BUF_PLANE1_PITCH_EXT, handle.stride, EGL_NONE,
        };
        static std::atomic<unsigned> imports{0};
        const bool diagnostic = imports.load(std::memory_order_relaxed) < 6 &&
            imports.fetch_add(1, std::memory_order_relaxed) < 6;
        int listed = -1;
        if (diagnostic) {
            auto query = real<PFNEGLQUERYDMABUFFORMATSEXTPROC>("eglQueryDmaBufFormatsEXT");
            EGLint formats[128], count = 0;
            if (query && query(egl, 128, formats, &count)) {
                listed = 0;
                for (EGLint index = 0; index < count && index < 128; ++index)
                    if (formats[index] == format) listed = 1;
            }
        }
        EGLImageKHR image = create_image(attrs);
        if (diagnostic) {
            std::fprintf(stderr, "AURORA_EGL_YUV_IMPORT fmt=%d size=%dx%d stride=%d listed=%d ok=%d\n",
                handle.format, handle.width, handle.height, handle.stride, listed, image != EGL_NO_IMAGE_KHR);
        }
        return image;
    }
    uint32_t format = handle.format == PIXEL_FMT_RGBA_8888 ? DRM_FORMAT_ABGR8888 :
        handle.format == PIXEL_FMT_RGBX_8888 ? DRM_FORMAT_XBGR8888 :
        handle.format == PIXEL_FMT_BGRA_8888 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    EGLint attrs[] = {EGL_WIDTH, handle.width, EGL_HEIGHT, handle.height, EGL_LINUX_DRM_FOURCC_EXT, int(format),
        EGL_DMA_BUF_PLANE0_FD_EXT, handle.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, handle.stride, EGL_NONE};
    return create_image(attrs);
}

// Content diagnostics for the RenderService composition target only. The
// marker and samples are bounded and never alter Mesa's backing history.
bool frame_samples_process()
{
    static const bool enabled = [] {
        if (!aurora_display_trace_enabled()) return false;
        const char *off = std::getenv("AURORA_FRAME_SAMPLES_OFF");
        if (off && !std::strcmp(off, "1")) return false;
        const char *forced = std::getenv("AURORA_FRAME_MARKER");
        if (forced && !std::strcmp(forced, "1")) return true;
        char name[32] = {};
        const int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const ssize_t length = read(fd, name, sizeof(name) - 1);
        close(fd);
        if (length > 0 && name[length - 1] == '\n') name[length - 1] = 0;
        return !std::strcmp(name, "render_service");
    }();
    return enabled;
}

EGLImageKHR blit_image(Window &window, const BufferHandle &buffer)
{
    struct stat identity {};
    if (fstat(buffer.fd, &identity)) return import_linear_buffer(window.display, buffer, "mesa_queue_blit");
    ++window.blit_uses;
    for (auto &entry : window.blit_images) {
        if (entry.dev == identity.st_dev && entry.ino == identity.st_ino && entry.width == buffer.width &&
            entry.height == buffer.height && entry.stride == buffer.stride && entry.format == buffer.format) {
            entry.used = window.blit_uses;
            return entry.image;
        }
    }
    EGLImageKHR image = import_linear_buffer(window.display, buffer, "mesa_queue_blit");
    if (image == EGL_NO_IMAGE_KHR) return image;
    constexpr size_t limit = 4;
    if (window.blit_images.size() >= limit) {
        auto oldest = std::min_element(window.blit_images.begin(), window.blit_images.end(),
            [](const BlitImage &a, const BlitImage &b) { return a.used < b.used; });
        auto destroy = real<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
        if (destroy) destroy(window.display, oldest->image);
        window.blit_images.erase(oldest);
    }
    window.blit_images.push_back({identity.st_dev, identity.st_ino, buffer.width, buffer.height, buffer.stride,
                                  buffer.format, image, window.blit_uses});
    return image;
}

// Stamp the frame serial into the top rows at the panel centre of the queued
// copy, below the camera housing. Uses the unhooked Mesa clear and restores
// all touched GL state.
void stamp_marker(Window &window)
{
    using namespace AuroraFrameSamples;
    if (!marker_fits(window.width, window.height)) return;
    auto clear = real<decltype(&glClear)>("glClear");
    if (!clear) return;
    GLint box[4]; GLfloat color[4]; GLboolean mask[4];
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const uint32_t serial = window.marker_serial & serial_mask;
    for (int cell = 0; cell < marker_cells; ++cell) {
        const float value = marker_bit(serial, cell) ? 1.f : 0.f;
        glClearColor(value, value, value, 1.f);
        glScissor(marker_left(window.width) + cell * marker_cell, 0, marker_cell, marker_rows);
        clear(GL_COLOR_BUFFER_BIT);
    }
    glClearColor(color[0], color[1], color[2], color[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor) glDisable(GL_SCISSOR_TEST);
    if (dither) glEnable(GL_DITHER);
}

bool blit_frame(Window &window, const BufferHandle &buffer, AuroraPerfTrace &trace)
{
    const char *enabled = std::getenv("AURORA_EGL_GPU_BLIT");
    if (!enabled || std::strcmp(enabled, "1") || window.width != buffer.width ||
        window.height != buffer.height || AuroraFrameCapture::token()) return false;
    const auto version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    if (!version || !strstr(version, "OpenGL ES 3.")) return false;
    auto get_config = real<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib");
    EGLint samples = 0;
    if (!get_config || !get_config(window.display, window.config, EGL_SAMPLES, &samples) || samples) return false;
    auto target = real<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>("glEGLImageTargetRenderbufferStorageOES");
    if (!target) return false;
    EGLImageKHR image = blit_image(window, buffer);
    if (image == EGL_NO_IMAGE_KHR) return false;

    GLint drawable, readable, renderbuffer_binding;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawable);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readable);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer_binding);
    GLuint renderbuffer = 0, framebuffer = 0;
    glGenRenderbuffers(1, &renderbuffer);
    glGenFramebuffers(1, &framebuffer);
    bool copied = false;
    if (renderbuffer && framebuffer) {
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        target(GL_RENDERBUFFER, image);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            GLint read_buffer;
            glGetIntegerv(GL_READ_BUFFER, &read_buffer);
            glReadBuffer(GL_BACK);
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            if (scissor) glDisable(GL_SCISSOR_TEST);
            // The window uses bottom-left GL coordinates; NativeWindow rows
            // start at the top. Keep the software copy's orientation.
            glBlitFramebuffer(0, window.height, window.width, 0, 0, 0, window.width, window.height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (scissor) glEnable(GL_SCISSOR_TEST);
            if (window.marked_frame) stamp_marker(window);
            trace.mark("gpu_blit");
            glFinish();
            trace.mark("gpu_blit_complete");
            glReadBuffer(read_buffer);
            copied = true;
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readable);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawable);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer_binding);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteRenderbuffers(1, &renderbuffer);
    return copied;
}

// Fingerprint the queued source after the blit completed; see frame_samples.h.
void sample_source(Window &window, const BufferHandle &buffer, EGLint age, const SwapDamage &damage,
                   const Region &region, unsigned long long submitted)
{
    static unsigned total = 0;  // protected by the WSI mutex
    if (total >= 20000 || buffer.width <= 0 || buffer.height <= 0 ||
        int64_t(buffer.stride) * buffer.height > buffer.size) return;
    struct stat identity {};
    if (fstat(buffer.fd, &identity)) return;
    const auto begin = aurora_display_time_ns();
    SourceMap *mapping = nullptr;
    for (auto &entry : window.source_maps)
        if (entry.dev == identity.st_dev && entry.ino == identity.st_ino && entry.size == size_t(buffer.size))
            mapping = &entry;
    if (!mapping) {
        void *map = mmap(nullptr, buffer.size, PROT_READ, MAP_SHARED, buffer.fd, 0);
        if (map == MAP_FAILED) return;
        if (window.source_maps.size() >= 6) {
            munmap(window.source_maps.front().map, window.source_maps.front().size);
            window.source_maps.erase(window.source_maps.begin());
        }
        window.source_maps.push_back({identity.st_dev, identity.st_ino, map, size_t(buffer.size)});
        mapping = &window.source_maps.back();
    }
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync)) return;
    const auto result = AuroraFrameSamples::sample(static_cast<const uint8_t *>(mapping->map), buffer.width,
                                                   buffer.height, buffer.stride, buffer.format);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync);
    ++total;
    char regions[AuroraFrameSamples::regions * 4 + 1];
    AuroraFrameSamples::format_regions(result, regions);
    const Region::Rect bounds = region.count ? region.rects[0] : Region::Rect{0, 0, 0, 0};
    aurora_display_trace("AURORA_SRC_SAMPLE serial=%u decoded=%d win=%p inode=%llu submit_ns=%llu age=%d"
        " swap_rects=%d swap_box=%d,%d,%u,%u set_rects=%d set_box=%d,%d,%d,%d cost_us=%llu regions=%s",
        window.marker_serial & AuroraFrameSamples::serial_mask, result.serial, static_cast<void *>(&window),
        static_cast<unsigned long long>(identity.st_ino), submitted, age, damage.count, bounds.x, bounds.y,
        bounds.w, bounds.h, window.declared_count, window.declared_damage[0], window.declared_damage[1],
        window.declared_damage[2], window.declared_damage[3], (aurora_display_time_ns() - begin) / 1000, regions);
}

// Preserve application pack state, including ES3 PBO/row offsets and read FBO.
// Application GL errors are not consumed by the bridge.
bool copy_frame(Window &window, BufferHandle &buffer, AuroraPerfTrace &trace)
{
    auto &pixels = window.readback;
    try { pixels.resize(size_t(window.width) * window.height * 4); }
    catch (...) { return false; }
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    bool es3 = version && (strstr(version, "OpenGL ES 3.") || strstr(version, "OpenGL ES 4."));
    GLint pack, row = 0, skip_rows = 0, skip_pixels = 0, pbo = 0, framebuffer = 0, read_buffer = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (es3) {
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row); glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels); glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer); glReadBuffer(GL_BACK);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    } else {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    trace.mark("readback_setup");
    glReadPixels(0, 0, window.width, window.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    trace.mark("gl_read_pixels");
    glFinish();
    trace.mark("gl_finish");
    window.capture.save(AuroraFrameCapture::token(), "egl", &window, pixels.data(),
        window.width, window.height, window.width*4, true, true);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (es3) {
        glReadBuffer(read_buffer); glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo); glPixelStorei(GL_PACK_ROW_LENGTH, row);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows); glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    } else glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    trace.mark("pack_restore_capture");
    void *memory = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
    trace.mark("queue_mmap");
    if (memory == MAP_FAILED) return false;
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync)) { munmap(memory, buffer.size); return false; }
    trace.mark("queue_sync_start");
    bool rgba = buffer.format == PIXEL_FMT_RGBA_8888 || buffer.format == PIXEL_FMT_RGBX_8888;
    for (int y = 0; y < buffer.height; ++y) {
        auto source = pixels.data() + size_t(window.height - 1 - uint64_t(y) * window.height / buffer.height) * window.width * 4;
        auto destination = static_cast<unsigned char *>(memory) + size_t(y) * buffer.stride;
        if (window.width == buffer.width) {
            AuroraCopyRow(destination,source,buffer.width,!rgba,false);
            continue;
        }
        for (int x = 0; x < buffer.width; ++x) {
            auto pixel = source + uint64_t(x) * window.width / buffer.width * 4;
            destination[x*4] = pixel[rgba ? 0 : 2]; destination[x*4+1] = pixel[1];
            destination[x*4+2] = pixel[rgba ? 2 : 0]; destination[x*4+3] = pixel[3];
        }
    }
    trace.mark("cpu_pixel_copy");
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    int result = ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(memory, buffer.size);
    trace.mark("queue_sync_end_unmap");
    return !result;
}

void reset_destroyed_backbuffer(Window &window)
{
    EGLint behavior=EGL_BUFFER_DESTROYED;
    auto query=real<decltype(&eglQuerySurface)>("eglQuerySurface");
    if (!query || !query(window.display,window.backing,EGL_SWAP_BEHAVIOR,&behavior)) return;
    if (window.age_aware && !window.fresh_backing && window.published_in_pool) {
        EGLint age = 0;
        if (query(window.display, window.backing, EGL_BUFFER_AGE_EXT, &age) &&
            age > 0 && static_cast<unsigned>(age) <= window.published_in_pool)
            return; // The renderer will repair this BO using its damage history.
    }
    if (!window.fresh_backing && behavior!=EGL_BUFFER_DESTROYED) return;
    // The copy WSI exposes age zero: the next frame has no valid old content.
    // The stock transparent client-composition pass blends layers without a
    // color clear. Give every invalid backbuffer deterministic transparent
    // contents, rather than allowing two alternating Mesa buffers to accumulate
    // earlier cursors and translucent Dock layers. Preserved surfaces retain
    // their contents. Never modify the frame that was just queued for scanout.
    const char *version=reinterpret_cast<const char *>(glGetString(GL_VERSION));
    bool es3=version && (strstr(version,"OpenGL ES 3.") || strstr(version,"OpenGL ES 4."));
    GLenum target=es3 ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER;
    GLint framebuffer=0; GLfloat color[4]={}; GLboolean write[4]={};
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,color);glGetBooleanv(GL_COLOR_WRITEMASK,write);
    glBindFramebuffer(target,0);glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(color[0],color[1],color[2],color[3]);glColorMask(write[0],write[1],write[2],write[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(target,framebuffer);
    window.fresh_backing=false;
    window.back_cleared=true;
}
}

namespace OHOS {
// OHOS appkit imports this frontend API while appspawn loads its ACE engine.
// Route the disk-cache hint to Mesa rather than retaining the Mali wrapper's
// BlobCache implementation. Mesa may legitimately omit caching in softpipe.
void EglSetCacheDir(const std::string dir)
{
    int result=dir.empty() ? unsetenv("MESA_SHADER_CACHE_DIR") :
        setenv("MESA_SHADER_CACHE_DIR",dir.c_str(),1);
    if (result) std::fprintf(stderr,"Aurora EGL: cannot configure Mesa shader cache: errno=%d\n",errno);
}
}

extern "C" EGLint eglGetError()
{
    if (local_error != EGL_SUCCESS) { EGLint code = local_error; local_error = EGL_SUCCESS; return code; }
    auto fn = real<decltype(&eglGetError)>("eglGetError"); return fn ? fn() : EGL_NOT_INITIALIZED;
}
extern "C" EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attributes)
{
    auto fn = real<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
    if (!fn) {
        record_graphics_diagnostic("render-service-egl.txt", "eglGetPlatformDisplayEXT symbol unavailable");
        error(EGL_NOT_INITIALIZED);
        return EGL_NO_DISPLAY;
    }
    if (platform != PLATFORM_OHOS) return fn(platform, native, attributes);
    if (attributes && attributes[0] != EGL_NONE) { error(EGL_BAD_ATTRIBUTE); return EGL_NO_DISPLAY; }
    std::lock_guard<std::mutex> lock(mutex);
    if (display.egl != EGL_NO_DISPLAY) return display.egl;
    const char *backend = getenv("AURORA_EGL_RENDER_BACKEND");
    bool asahi = backend && !strcmp(backend, "asahi");
    AuroraDrmDisplay kms = {};
    int fd = asahi ? open_asahi_render_node() : (AuroraOpenDisplayDrm(&kms, 0) ? -1 : kms.fd);
    if (fd < 0) {
        record_graphics_diagnostic("render-service-egl.txt", asahi ? "open Asahi DRM render node failed" : "open KMS DRM device failed");
        error(EGL_NOT_INITIALIZED);
        return EGL_NO_DISPLAY;
    }
    auto device = gbm_create_device(fd);
    if (!device) {
        record_graphics_diagnostic("render-service-egl.txt", "gbm_create_device failed");
        close(fd);
        error(EGL_NOT_INITIALIZED);
        return EGL_NO_DISPLAY;
    }
    display.egl = fn(EGL_PLATFORM_GBM_KHR, device, nullptr);
    if (display.egl == EGL_NO_DISPLAY) {
        record_graphics_diagnostic("render-service-egl.txt", "Mesa GBM EGL display unavailable");
        gbm_device_destroy(device);
        close(fd);
        return EGL_NO_DISPLAY;
    }
    display.device = device;
    display.asahi = asahi;
    return display.egl;
}
extern "C" EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attributes)
{
    if (platform == PLATFORM_OHOS) {
        if (attributes && attributes[0] != EGL_NONE) { error(EGL_BAD_ATTRIBUTE); return EGL_NO_DISPLAY; }
        return eglGetPlatformDisplayEXT(platform, native, nullptr);
    }
    auto fn = real<decltype(&eglGetPlatformDisplay)>("eglGetPlatformDisplay");
    return fn ? fn(platform, native, attributes) : EGL_NO_DISPLAY;
}
extern "C" EGLDisplay eglGetDisplay(EGLNativeDisplayType native)
{ return eglGetPlatformDisplayEXT(PLATFORM_OHOS, reinterpret_cast<void *>(native), nullptr); }

extern "C" EGLSurface eglCreateWindowSurface(EGLDisplay egl, EGLConfig config, EGLNativeWindowType native, const EGLint *attributes)
{
    if (egl != display.egl) {
        auto fn = real<decltype(&eglCreateWindowSurface)>("eglCreateWindowSurface");
        return fn ? fn(egl, config, native, attributes) : EGL_NO_SURFACE;
    }
    auto api = native_api();
    if (!api || !native) { error(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
    auto window = std::make_shared<Window>(); window->display = egl; window->config = config;
    auto object = reinterpret_cast<NativeWindow *>(native);
    if (api->reference(object)) { error(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
    window->native = object;
    window->scanout_format = is_stock_scanout_window(object);
    if (window->scanout_format) {
        configure_scanout_format(*window);
        std::fprintf(stderr, "AURORA_FRAMEBUFFER_FORMAT configured=BGRA8888\n");
    }
    uint64_t usage=0;
    int width=0, height=0;
    int usage_get=api->option(object, 4, &usage);
    int usage_set=(usage_get || (usage & HBM_USE_PROTECTED)) ? -1 :
        api->option(object, 5, usage | HBM_USE_CPU_READ | HBM_USE_CPU_WRITE | HBM_USE_HW_RENDER);
    bool size_valid=geometry(object,width,height);
    if (usage_get || (usage & HBM_USE_PROTECTED) || usage_set) {
        std::fprintf(stderr,"AURORA_EGL_WINDOW_REJECT get=%d set=%d usage=%llu geometry=%dx%d valid=%d\n",
                     usage_get,usage_set,static_cast<unsigned long long>(usage),width,height,size_valid);
        error(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE;
    }
    if (attributes) {
        for (size_t i=0;attributes[i]!=EGL_NONE;i+=2) {
            if (i>=128) {error(EGL_BAD_ATTRIBUTE);return EGL_NO_SURFACE;}
            window->creation_attributes.push_back(attributes[i]);
            window->creation_attributes.push_back(attributes[i+1]);
        }
        window->creation_attributes.push_back(EGL_NONE);
    }
    std::lock_guard<std::mutex> lock(mutex);
    // Stock RS calls CreateEGLSurface before SET_BUFFER_GEOMETRY. Preserve the
    // logical surface now and allocate its real backing at first MakeCurrent,
    // after the producer has provided its dimensions. No invented 1x1 buffer.
    if (size_valid && !create_backing(*window, width, height, attributes)) {
        std::fprintf(stderr,"AURORA_EGL_BACKING_REJECT geometry=%dx%d\n",width,height);
        return EGL_NO_SURFACE;
    }
    auto handle = reinterpret_cast<EGLSurface>(window.get());
    windows.emplace(handle, window); return handle;
}
extern "C" EGLBoolean eglMakeCurrent(EGLDisplay egl, EGLSurface draw, EGLSurface read, EGLContext context)
{
    auto fn = real<decltype(&eglMakeCurrent)>("eglMakeCurrent"); if (!fn) return error(EGL_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex);
    auto d = find(draw), r = find(read);
    for (auto window : {d,r}) {
        if (!window || window->backing!=EGL_NO_SURFACE) continue;
        int width=0,height=0;
        if (!queue_geometry(window->native,width,height) ||
            !create_backing(*window,width,height,window->creation_attributes.empty() ? nullptr : window->creation_attributes.data())) {
            return error(EGL_BAD_NATIVE_WINDOW);
        }
    }
    if (!fn(egl, backing(draw), backing(read), context)) return EGL_FALSE;
    if (!display.renderer_reported && egl == display.egl && context != EGL_NO_CONTEXT &&
        (display.asahi || getenv("AURORA_RENDER_DIAGNOSTICS_DIR"))) {
        const auto renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        if (renderer) {
            if (display.asahi) std::fprintf(stderr, "AURORA_EGL_ASAHI_GL_RENDERER=%s\n", renderer);
            record_graphics_diagnostic("render-service-renderer.txt", renderer);
            display.renderer_reported = true;
        }
    }
    if (d && d->fresh_backing) reset_destroyed_backbuffer(*d);
    current_draw = d; current_read = r; return EGL_TRUE;
}
extern "C" EGLSurface eglGetCurrentSurface(EGLint which)
{
    if (which == EGL_DRAW && current_draw) return reinterpret_cast<EGLSurface>(current_draw.get());
    if (which == EGL_READ && current_read) return reinterpret_cast<EGLSurface>(current_read.get());
    auto fn = real<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface"); return fn ? fn(which) : EGL_NO_SURFACE;
}
extern "C" EGLBoolean eglDestroySurface(EGLDisplay egl, EGLSurface surface)
{
    auto fn = real<decltype(&eglDestroySurface)>("eglDestroySurface"); if (!fn) return error(EGL_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex); auto window = find(surface);
    if (!window) return fn(egl, surface);
    if (egl!=window->display) return error(EGL_BAD_MATCH);
    // A current window keeps its Mesa surface until the last thread unbinds,
    // following EGL's deferred destruction rule. Window releases in GBM order.
    window->destroyed = true;
    windows.erase(surface);
    deferred_windows.erase(std::remove_if(deferred_windows.begin(), deferred_windows.end(),
        [](const auto &entry) { return entry.expired(); }), deferred_windows.end());
    if (window.use_count() > 1) deferred_windows.emplace_back(window);
    return EGL_TRUE;
}
extern "C" EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay egl, EGLConfig config, void *native, const EGLint *attributes)
{ return eglCreateWindowSurface(egl, config, reinterpret_cast<EGLNativeWindowType>(native), attributes); }
extern "C" EGLSurface eglCreatePlatformWindowSurface(EGLDisplay egl, EGLConfig config, void *native, const EGLAttrib *attributes)
{
    std::vector<EGLint> converted;
    if (attributes) for (size_t n = 0; attributes[n] != EGL_NONE; n += 2) {
        if (n > 128 || attributes[n] != EGLint(attributes[n]) || attributes[n+1] != EGLint(attributes[n+1])) {
            error(EGL_BAD_ATTRIBUTE); return EGL_NO_SURFACE;
        }
        converted.push_back(attributes[n]); converted.push_back(attributes[n+1]);
    }
    converted.push_back(EGL_NONE);
    return eglCreateWindowSurface(egl, config, reinterpret_cast<EGLNativeWindowType>(native), converted.data());
}
extern "C" EGLBoolean eglQuerySurface(EGLDisplay egl, EGLSurface surface, EGLint attribute, EGLint *value)
{
    if (!value) return error(EGL_BAD_PARAMETER);
    std::lock_guard<std::mutex> lock(mutex); auto window = find(surface);
    if (window) {
        if (egl != window->display) return error(EGL_BAD_MATCH);
        if (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) {
            int width, height;
            if (!queue_geometry(window->native, width, height)) return error(EGL_BAD_NATIVE_WINDOW);
            *value = attribute == EGL_WIDTH ? width : height; return EGL_TRUE;
        }
        if (attribute == EGL_BUFFER_AGE_EXT) {
            *value = 0;
            if (!front_enabled()) return EGL_TRUE; // preserve the manual copy path's age-zero contract
            window->age_aware = true;
            auto query = real<decltype(&eglQuerySurface)>("eglQuerySurface");
            int width = 0, height = 0;
            if (current_draw == window && window->backing != EGL_NO_SURFACE &&
                geometry(window->native, width, height) && width == window->width && height == window->height &&
                !window->front_recover_backing) {
                EGLint age = 0;
                if (query && query(egl, window->backing, attribute, &age) && !window->back_cleared &&
                    age > 0 && static_cast<unsigned>(age) <= window->published_in_pool) *value = age;
            }
            if (window->swaps < 3) std::fprintf(stderr,"AURORA_EGL_AGE id=%p swaps=%u age=%d\n",window.get(),window->swaps,*value);
            if (aurora_display_trace_enabled()) window->trace_age = *value;
            return EGL_TRUE;
        }
    }
    auto fn = real<decltype(&eglQuerySurface)>("eglQuerySurface"); return fn ? fn(egl, backing(surface), attribute, value) : error(EGL_NOT_INITIALIZED);
}
static EGLBoolean publish_window(EGLDisplay egl, EGLSurface surface, const SwapDamage &requested)
{
    AuroraPerfTrace trace("egl-swap");
    std::lock_guard<std::mutex> lock(mutex); trace.mark("lock"); auto window = find(surface);
    auto swap = real<decltype(&eglSwapBuffers)>("eglSwapBuffers");
    if (!swap) return error(EGL_NOT_INITIALIZED);
    if (!window) {
        auto fn = requested.extension ? real<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(requested.extension) : nullptr;
        return fn ? fn(egl, surface, requested.rects, requested.count) : swap(egl, surface);
    }
    if (egl != window->display || current_draw != window || current_read != window) return error(EGL_BAD_CURRENT_SURFACE);
    const SwapDamage damage = front_enabled() ? requested : SwapDamage{nullptr, 0, nullptr};
    trace.object(window.get(),window->width,window->height);
    trace_queue_origin(*window);
    NativeBuffer *buffer = nullptr; int fence = -1;
    auto api = native_api();
    configure_scanout_format(*window);
    if (auto front = try_front_swap(*window, swap, trace, damage)) return *front;
    if (api->request(window->native, &buffer, &fence)) {
        if (fence >= 0) close(fence);
        window->invalidate_history();
        return error(EGL_BAD_ALLOC);
    }
    trace.mark("request_buffer");
    auto handle = api->handle(buffer);
    bool waited = wait_fence(fence);
    trace.mark("acquire_fence");
    ++window->swaps;
    if (window->swaps <= 3 || window->swaps%120 == 0) {
        EGLint age=-1, behavior=-1;
        auto query=real<decltype(&eglQuerySurface)>("eglQuerySurface");
        query(egl,window->backing,EGL_BUFFER_AGE_EXT,&age);
        query(egl,window->backing,EGL_SWAP_BEHAVIOR,&behavior);
        std::fprintf(stderr,"AURORA_EGL_SWAP pid=%d id=%p n=%u backing=%dx%d queue=%dx%d fd=%d waited=%d mesa_age=%d behavior=0x%x clear_calls=%u\n",
            getpid(),window.get(),window->swaps,window->width,window->height,
            handle ? handle->width : 0,handle ? handle->height : 0,handle ? handle->fd : -1,waited,age,behavior,clear_calls);
    }
    const EGLint given_age = window->trace_age;
    // Only the full-panel composition target; small RenderService surfaces
    // could place the marker outside the camera housing.
    static const bool forced = [] { const char *v = std::getenv("AURORA_FRAME_MARKER"); return v && !std::strcmp(v, "1"); }();
    window->marked_frame = waited && valid(handle) && !window->scanout_backing && frame_samples_process() &&
        window->width == handle->width && window->height == handle->height &&
        (forced || int64_t(window->width) * window->height >= 1000000);
    if (window->marked_frame) window->marker_serial = (window->marker_serial + 1) & AuroraFrameSamples::serial_mask;
    if (!waited || !valid(handle) ||
        (!blit_frame(*window, *handle, trace) && !copy_frame(*window, *handle, trace))) {
        window->marked_frame = false;
        api->abort(window->native, buffer); window->invalidate_history(); return error(EGL_BAD_ALLOC);
    }
    Region::Rect rects{};
    const bool same_size = window->width == handle->width && window->height == handle->height;
    Region region = same_size ? damage.native(*window, rects) : Region{nullptr, 0};
    // A marked copy-path frame has no stamp; its sample records decoded=-1.
    const auto submitted = window->marked_frame ? aurora_display_time_ns() : 0;
    if (window->marked_frame) sample_source(*window, *handle, given_age, damage, region, submitted);
    window->marked_frame = false;
    window->declared_count = -1;
    if (api->flush(window->native, buffer, -1, region)) {
        api->abort(window->native, buffer); window->invalidate_history(); return error(EGL_BAD_NATIVE_WINDOW);
    }
    trace.mark("flush_native");
    // The retiring surface has exported buffers in the stock queue. After
    // publishing a copied frame, move rendering to a separate backing instead
    // of asking Mesa to recycle those locked front buffers.
    if (!window->front_recover_backing && !damage.swap(*window, swap)) {
        window->invalidate_history();
        return EGL_FALSE;
    }
    trace.mark("mesa_swap");
    int width, height;
    if (!queue_geometry(window->native, width, height) || !resize(*window, width, height)) {
        window->invalidate_history();
        return error(EGL_BAD_ALLOC);
    }
    trace.mark("geometry_resize");
    if (window->fresh_backing) reset_destroyed_backbuffer(*window);
    else did_publish(*window);
    trace.mark("clear_next_backbuffer");
    trace.result(0);
    return EGL_TRUE;
}
extern "C" EGLBoolean eglSwapBuffers(EGLDisplay egl, EGLSurface surface)
{ return publish_window(egl, surface, {nullptr, 0, nullptr}); }
extern "C" EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay egl, EGLSurface surface, const EGLint *rects, EGLint count)
{ return valid_damage(rects, count) ? publish_window(egl, surface, {rects, count, "eglSwapBuffersWithDamageKHR"}) : error(EGL_BAD_PARAMETER); }
extern "C" EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay egl, EGLSurface surface, const EGLint *rects, EGLint count)
{ return valid_damage(rects, count) ? publish_window(egl, surface, {rects, count, "eglSwapBuffersWithDamageEXT"}) : error(EGL_BAD_PARAMETER); }
extern "C" EGLBoolean eglSetDamageRegionKHR(EGLDisplay egl, EGLSurface surface, EGLint *rects, EGLint count)
{
    if (!valid_damage(rects, count)) return error(EGL_BAD_PARAMETER);
    std::lock_guard<std::mutex> lock(mutex);
    auto window = find(surface);
    auto fn = real<PFNEGLSETDAMAGEREGIONKHRPROC>("eglSetDamageRegionKHR");
    if (!window) return fn ? fn(egl, surface, rects, count) : error(EGL_BAD_SURFACE);
    if (window->display != egl || current_draw != window) return error(EGL_BAD_SURFACE);
    if (aurora_display_trace_enabled()) {
        int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (EGLint i = 0; i < count; ++i) {
            const EGLint *r = rects + 4 * i;
            x0 = std::min(x0, r[0]); y0 = std::min(y0, r[1]);
            x1 = std::max<int64_t>(x1, int64_t(r[0]) + r[2]); y1 = std::max<int64_t>(y1, int64_t(r[1]) + r[3]);
        }
        window->declared_count = count;
        if (count) { window->declared_damage[0] = x0; window->declared_damage[1] = y0;
                     window->declared_damage[2] = x1 - x0; window->declared_damage[3] = y1 - y0; }
    }
    // Mesa owns preservation of the GL backbuffer. On a fresh or explicitly
    // cleared buffer the caller saw age zero, so keep the entire frame valid.
    auto query = real<decltype(&eglQueryString)>("eglQueryString");
    const char *extensions = query ? query(egl, EGL_EXTENSIONS) : nullptr;
    if (!front_enabled() || !fn || !extensions || !std::strstr(extensions, "EGL_KHR_partial_update") ||
        window->back_cleared || !window->published_in_pool) return EGL_TRUE;
    return fn(egl, window->backing, rects, count);
}

extern "C" EGLBoolean eglSurfaceAttrib(EGLDisplay egl, EGLSurface surface, EGLint attribute, EGLint value)
{ std::lock_guard<std::mutex> lock(mutex); auto fn = real<decltype(&eglSurfaceAttrib)>("eglSurfaceAttrib");
  std::fprintf(stderr,"AURORA_EGL_ATTR pid=%d id=%p attribute=0x%x value=0x%x\n",getpid(),surface,attribute,value);
  return fn ? fn(egl, backing(surface), attribute, value) : error(EGL_NOT_INITIALIZED); }
extern "C" EGLBoolean eglBindTexImage(EGLDisplay egl, EGLSurface surface, EGLint buffer)
{ std::lock_guard<std::mutex> lock(mutex); auto fn = real<decltype(&eglBindTexImage)>("eglBindTexImage");
  return fn ? fn(egl, backing(surface), buffer) : error(EGL_NOT_INITIALIZED); }
extern "C" EGLBoolean eglReleaseTexImage(EGLDisplay egl, EGLSurface surface, EGLint buffer)
{ std::lock_guard<std::mutex> lock(mutex); auto fn = real<decltype(&eglReleaseTexImage)>("eglReleaseTexImage");
  return fn ? fn(egl, backing(surface), buffer) : error(EGL_NOT_INITIALIZED); }
extern "C" EGLBoolean eglCopyBuffers(EGLDisplay egl, EGLSurface surface, EGLNativePixmapType pixmap)
{ std::lock_guard<std::mutex> lock(mutex); auto fn = real<decltype(&eglCopyBuffers)>("eglCopyBuffers");
  return fn ? fn(egl, backing(surface), pixmap) : error(EGL_NOT_INITIALIZED); }

extern "C" EGLImageKHR eglCreateImageKHR(EGLDisplay egl, EGLContext context, EGLenum target, EGLClientBuffer object, const EGLint *attributes)
{
    auto fn = real<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR"); if (!fn) { error(EGL_NOT_INITIALIZED); return EGL_NO_IMAGE_KHR; }
    if (target != NATIVE_BUFFER_OHOS) {
        const auto image = fn(egl, context, target, object, attributes);
        if (image == EGL_NO_IMAGE_KHR && target == EGL_LINUX_DMA_BUF_EXT && egl == display.egl)
            trace_dma_buf_failure(attributes);
        return image;
    }
    if (context != EGL_NO_CONTEXT) { error(EGL_BAD_CONTEXT); return EGL_NO_IMAGE_KHR; }
    if (attributes) for (int n = 0; attributes[n] != EGL_NONE; n += 2) {
        if (attributes[n] != EGL_IMAGE_PRESERVED_KHR ||
            (attributes[n+1] != EGL_TRUE && attributes[n+1] != EGL_FALSE)) {
            error(EGL_BAD_ATTRIBUTE); return EGL_NO_IMAGE_KHR;
        }
    }
    auto api = native_api(); auto handle = api ? api->handle(reinterpret_cast<NativeBuffer *>(object)) : nullptr;
    if (!handle || (!valid(handle) && !AuroraValidLinearYuv420(*handle))) {
        if (handle)
            trace_image_failure("native_handle_rejected", handle->fd, handle->width, handle->height,
                handle->format, handle->stride, 0, handle->size, handle->usage);
        error(EGL_BAD_PARAMETER); return EGL_NO_IMAGE_KHR;
    }
    return import_linear_buffer(egl, *handle, "mesa_native_image");
}
extern "C" EGLBoolean eglReleaseThread()
{
    auto fn = real<decltype(&eglReleaseThread)>("eglReleaseThread"); if (!fn) return error(EGL_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex);
    if (!fn()) return EGL_FALSE;
    current_draw.reset(); current_read.reset(); return EGL_TRUE;
}
extern "C" EGLImage eglCreateImage(EGLDisplay egl, EGLContext context, EGLenum target, EGLClientBuffer object, const EGLAttrib *attributes)
{
    if (target != NATIVE_BUFFER_OHOS) {
        auto fn = real<decltype(&eglCreateImage)>("eglCreateImage");
        const auto image = fn ? fn(egl, context, target, object, attributes) : EGL_NO_IMAGE;
        if (image == EGL_NO_IMAGE && target == EGL_LINUX_DMA_BUF_EXT && egl == display.egl)
            trace_dma_buf_failure(attributes);
        return image;
    }
    std::vector<EGLint> converted;
    if (attributes) for (size_t n = 0; attributes[n] != EGL_NONE; n += 2) {
        if (n > 128 || attributes[n] != EGLint(attributes[n]) || attributes[n+1] != EGLint(attributes[n+1])) {
            error(EGL_BAD_ATTRIBUTE); return EGL_NO_IMAGE;
        }
        converted.push_back(attributes[n]); converted.push_back(attributes[n+1]);
    }
    converted.push_back(EGL_NONE);
    return reinterpret_cast<EGLImage>(eglCreateImageKHR(egl, context, target, object, converted.data()));
}

// Functions whose arguments contain no virtual NativeWindow surface handles.
extern "C" EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    auto fn = real<decltype(&eglInitialize)>("eglInitialize");
    EGLBoolean result = fn ? fn(d, major, minor) : EGL_FALSE;
    if (d != EGL_NO_DISPLAY && getenv("AURORA_RENDER_DIAGNOSTICS_DIR")) {
        std::lock_guard<std::mutex> lock(mutex);
        if (d == display.egl)
            record_graphics_diagnostic("render-service-egl.txt", result ? "eglInitialize succeeded" : "eglInitialize failed");
    }
    return result;
}

#define FORWARD(ret, name, signature, arguments, fallback) \
extern "C" ret name signature { auto fn = real<decltype(&name)>(#name); return fn ? fn arguments : fallback; }
extern "C" EGLBoolean eglTerminate(EGLDisplay d)
{
    auto fn = real<decltype(&eglTerminate)>("eglTerminate");
    if (!fn) return error(EGL_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex);
    // eglTerminate destroys non-current Mesa surfaces and marks current
    // surfaces for destruction at unbind. The release_buffer hook may only
    // run while its Mesa surface-private storage is live.
    auto retire = [](Window &w) {
        w.clear_fronts(w.gbm);
        w.release_blit_images();
        w.backing = EGL_NO_SURFACE;
        w.destroyed = true;
    };
    for (auto it = windows.begin(); it != windows.end(); ) {
        if (it->second->display != d) { ++it; continue; }
        retire(*it->second);
        ++it;
    }
    for (auto &entry : deferred_windows) {
        auto w = entry.lock();
        if (w && w->display == d) retire(*w);
    }
    // Keep native GBM surfaces alive through the Mesa call itself.
    EGLBoolean result = fn(d);
    for (auto it = windows.begin(); it != windows.end(); ) {
        if (it->second->display == d) it = windows.erase(it); else ++it;
    }
    return result;
}
FORWARD(EGLBoolean, eglChooseConfig, (EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint s, EGLint *n), (d,a,c,s,n), EGL_FALSE)
FORWARD(EGLBoolean, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint s, EGLint *n), (d,c,s,n), EGL_FALSE)
FORWARD(EGLBoolean, eglGetConfigAttrib, (EGLDisplay d, EGLConfig c, EGLint a, EGLint *v), (d,c,a,v), EGL_FALSE)
FORWARD(EGLContext, eglCreateContext, (EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a), (d,c,s,a), EGL_NO_CONTEXT)
FORWARD(EGLBoolean, eglDestroyContext, (EGLDisplay d, EGLContext c), (d,c), EGL_FALSE)
FORWARD(EGLBoolean, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d,c,a,v), EGL_FALSE)
FORWARD(EGLContext, eglGetCurrentContext, (), (), EGL_NO_CONTEXT)
FORWARD(EGLDisplay, eglGetCurrentDisplay, (), (), EGL_NO_DISPLAY)
FORWARD(EGLBoolean, eglBindAPI, (EGLenum a), (a), EGL_FALSE)
FORWARD(EGLenum, eglQueryAPI, (), (), EGL_NONE)
FORWARD(EGLBoolean, eglWaitClient, (), (), EGL_FALSE)
FORWARD(EGLBoolean, eglWaitGL, (), (), EGL_FALSE)
FORWARD(EGLBoolean, eglWaitNative, (EGLint e), (e), EGL_FALSE)
FORWARD(EGLBoolean, eglSwapInterval, (EGLDisplay d, EGLint i), (d,i), EGL_FALSE)
FORWARD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay d, EGLConfig c, const EGLint *a), (d,c,a), EGL_NO_SURFACE)
FORWARD(EGLSurface, eglCreatePixmapSurface, (EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a), (d,c,p,a), EGL_NO_SURFACE)
FORWARD(EGLBoolean, eglDestroyImageKHR, (EGLDisplay d, EGLImageKHR i), (d,i), EGL_FALSE)
FORWARD(EGLBoolean, eglDestroyImage, (EGLDisplay d, EGLImage i), (d,i), EGL_FALSE)
FORWARD(EGLSyncKHR, eglCreateSyncKHR, (EGLDisplay d, EGLenum t, const EGLint *a), (d,t,a), EGL_NO_SYNC_KHR)
FORWARD(EGLBoolean, eglDestroySyncKHR, (EGLDisplay d, EGLSyncKHR s), (d,s), EGL_FALSE)
FORWARD(EGLint, eglClientWaitSyncKHR, (EGLDisplay d, EGLSyncKHR s, EGLint f, EGLTimeKHR t), (d,s,f,t), EGL_FALSE)
FORWARD(EGLBoolean, eglGetSyncAttribKHR, (EGLDisplay d, EGLSyncKHR s, EGLint a, EGLint *v), (d,s,a,v), EGL_FALSE)
FORWARD(EGLint, eglWaitSyncKHR, (EGLDisplay d, EGLSyncKHR s, EGLint f), (d,s,f), EGL_FALSE)
// NativeImage links this extension directly, even for its CPU consumer path.
// Delegate to Mesa; an unsupported native fence must report an EGL error.
extern "C" EGLint eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR s)
{
    auto fn = real<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");
    if (fn) return fn(d, s);
    error(EGL_BAD_MATCH);
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
}
// Stock NativeImage and RenderService import this GLES extension directly;
// Mesa's GLES dispatch exports it through eglGetProcAddress instead.
extern "C" void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
    auto fn = real<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
    if (!fn) {
        std::fprintf(stderr, "Aurora EGL: required Mesa image-target dispatch is unavailable\n");
        std::abort();
    }
    fn(target, image);
}

// Bounded tracing of the actual GL clear submitted by the stock drawing
// engine. This delegates unchanged and does not consume application GL errors.
extern "C" void glClear(GLbitfield mask)
{
    auto fn=real<decltype(&glClear)>("glClear");
    if (!fn) std::abort();
    ++clear_calls;
    if (clear_calls<=20 || (clear_calls%240==0)) {
        GLint fbo=0,scissor[4]={}; GLfloat color[4]={}; GLboolean write[4]={};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fbo);
        glGetIntegerv(GL_SCISSOR_BOX,scissor);glGetFloatv(GL_COLOR_CLEAR_VALUE,color);
        glGetBooleanv(GL_COLOR_WRITEMASK,write);
        std::fprintf(stderr,"AURORA_GL_CLEAR pid=%d id=%p n=%u mask=0x%x fbo=%d scissor_enabled=%d scissor=%d,%d,%d,%d color=%g,%g,%g,%g write=%d%d%d%d\n",
            getpid(),current_draw.get(),clear_calls,mask,fbo,glIsEnabled(GL_SCISSOR_TEST),
            scissor[0],scissor[1],scissor[2],scissor[3],color[0],color[1],color[2],color[3],write[0],write[1],write[2],write[3]);
    }
    fn(mask);
}

extern "C" const char *eglQueryString(EGLDisplay egl, EGLint name)
{
    auto fn = real<decltype(&eglQueryString)>("eglQueryString");
    const char *base = fn ? fn(egl, name) : nullptr;
    if (!base || name != EGL_EXTENSIONS || (egl != EGL_NO_DISPLAY && egl != display.egl)) return base;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = extension_strings.find(egl);
    if (it == extension_strings.end()) {
        std::string extensions(base);
        extensions += egl == EGL_NO_DISPLAY ? " EGL_KHR_platform_ohos" : " EGL_OHOS_image_native_buffer";
        it = extension_strings.emplace(egl, std::move(extensions)).first;
    }
    return it->second.c_str();
}

extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
    if (!name) return nullptr;
#define INTERCEPT(function) if (!strcmp(name, #function)) return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&function)
    INTERCEPT(eglGetPlatformDisplay); INTERCEPT(eglGetPlatformDisplayEXT); INTERCEPT(eglGetDisplay);
    INTERCEPT(eglCreateWindowSurface); INTERCEPT(eglDestroySurface); INTERCEPT(eglQuerySurface);
    INTERCEPT(eglCreatePlatformWindowSurfaceEXT); INTERCEPT(eglCreatePlatformWindowSurface);
    INTERCEPT(eglMakeCurrent); INTERCEPT(eglGetCurrentSurface); INTERCEPT(eglSwapBuffers);
    INTERCEPT(eglDupNativeFenceFDANDROID);
    INTERCEPT(glEGLImageTargetTexture2DOES);
    INTERCEPT(glClear);
    INTERCEPT(eglSurfaceAttrib); INTERCEPT(eglBindTexImage); INTERCEPT(eglReleaseTexImage); INTERCEPT(eglCopyBuffers);
    INTERCEPT(eglSwapBuffersWithDamageKHR); INTERCEPT(eglSwapBuffersWithDamageEXT); INTERCEPT(eglSetDamageRegionKHR);
    INTERCEPT(eglCreateImageKHR); INTERCEPT(eglCreateImage); INTERCEPT(eglReleaseThread); INTERCEPT(eglGetError);
    INTERCEPT(eglQueryString); INTERCEPT(eglTerminate);
#undef INTERCEPT
    auto fn = real<decltype(&eglGetProcAddress)>("eglGetProcAddress"); return fn ? fn(name) : nullptr;
}
