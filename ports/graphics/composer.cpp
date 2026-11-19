/* SPDX-License-Identifier: GPL-3.0-only
 * Client-composition backend for up to three physical outputs. RenderService composes the full
 * client target; this library can import it into KMS or use linear copy buffers.
 * No vendor GPU process, ION, fbdev or Linux desktop session is used.
 */
#include "include/composer_abi.h"
#include "include/frame_diagnostics.h"
#include "include/frame_samples.h"
#include "include/perf_trace.h"
#include "include/pixel_copy.h"
#include "include/boot_handoff.h"
#include "include/buffer_handle.h"
#include "include/display_type.h"
#include "include/aurora_drm.h"
#include "../hardware/backlight.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <linux/dma-buf.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

using namespace AuroraComposer;
namespace {
constexpr int OK = 0, FAIL = -1, PARAM = -3, UNSUPPORTED = -5;
struct Scanout { int dma_fd = -1; uint32_t gem = 0, fb = 0, pitch = 0; uint64_t size = 0; void *map = MAP_FAILED; };
struct ImportedScanout {
    uint32_t fb = 0;
    int fd = -1;
    BufferHandle buffer = {};
    dev_t dev = 0;
    ino_t ino = 0;
    // Read-only diagnostic mapping, created on the first sampled flip.
    void *map = MAP_FAILED;
    // Fingerprint taken when this buffer was last flipped on screen.
    bool sampled = false;
    AuroraFrameSamples::Result shown;
};
struct ScanSample {
    bool valid = false;
    uint32_t fb = 0;
    unsigned long long inode = 0, sampled_ns = 0, cost_ns = 0;
    AuroraFrameSamples::Result result;
};
struct State {
    std::mutex mutex;
    std::mutex vsync_mutex, flip_mutex;
    std::condition_variable flip_wake;
    std::condition_variable wake;
    AuroraDrmDisplay drm = {};
    Scanout buffers[2];
    drmModeCrtc *saved = nullptr;
    int front = -1, pending_slot = -1, client_fd = -1;
    std::array<ImportedScanout, 4> imports{};
    size_t next_import = 0;
    ImportedScanout *imported_front = nullptr, *imported_pending = nullptr;
    uint32_t primary_plane = 0, plane_fb_property = 0, crtc_out_fence_property = 0;
    // A direct flip returned to HdiOutput before KMS completed it; its
    // completion is consumed by the next Commit, Clear or power change.
    bool async_flip = false;
    unsigned async_flips = 0;
    AuroraBootHandoff boot_handoff;
    AuroraFrameCapture client_capture, scanout_capture;
    bool handoff_held_frame = false, direct_reported = false, fb_reject_reported = false;
    bool boot_direct_reported = false;
    unsigned frame = 0;
    uint32_t device = 0, flip_sequence = 0;
    uint64_t event_ns = 0, flips = 0;
    bool flip_delivered = false;
    bool tried_atomic_plane = false;
    bool pending = false, enabled = false, stop = false;
    bool flip_clock_monotonic = false;
    uint64_t last_flip_ns = 0;
    ScanSample scan_sample;
    unsigned scan_samples = 0, retire_checked = 0, retire_changed = 0;
    BufferHandle client = {};
    Rect crop = {};
    std::map<uint32_t, int32_t> layers;
    uint32_t next_layer = 1;
    Vblank vblank = nullptr;
    void *vblank_data = nullptr;
    uint64_t vsync_period_ns = 0;
    unsigned vsync_generation = 0;
    std::thread ticker;
    State() { drm.fd = -1; }
    ~State();
};

std::mutex topology_mutex, prime_mutex;
std::array<State, 3> outputs;
unsigned output_count = 0;

struct FlipDispatcher {
    int stop_fd = -1;
    std::thread thread;
    bool start(int drm_fd);
    ~FlipDispatcher();
} dispatcher;

bool env_enabled(const char *name)
{
    const char *value = std::getenv(name);
    return value && !std::strcmp(value, "1");
}

void kms_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void kms_log(const char *format, ...)
{
    char message[384];
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(message) - 1) return;
    using Print = int (*)(int, int, unsigned, const char *, const char *, ...);
    static Print print = [] {
        auto function = reinterpret_cast<Print>(dlsym(RTLD_DEFAULT, "HiLogPrint"));
        if (function) return function;
        void *library = dlopen("libhilog.so", RTLD_NOW | RTLD_LOCAL);
        return library ? reinterpret_cast<Print>(dlsym(library, "HiLogPrint")) : nullptr;
    }();
    constexpr int log_core = 3, log_info = 4;
    if (print) print(log_core, log_info, 0xD001400, "AuroraKMS", "%{public}s", message);
    message[size] = '\n';
    (void)write(STDERR_FILENO, message, size + 1);
}

bool boot_parameter(const char *key)
{
    using Getter = int (*)(const char *, const char *, char *, unsigned);
    static Getter getter = [] {
        auto function = reinterpret_cast<Getter>(dlsym(RTLD_DEFAULT, "GetParameter"));
        if (!function) {
            void *library = dlopen("libbegetutil.z.so", RTLD_NOW | RTLD_LOCAL);
            if (library) function = reinterpret_cast<Getter>(dlsym(library, "GetParameter"));
        }
        return function;
    }();
    char value[16] = {};
    return getter && getter(key, "", value, sizeof(value)) > 0 &&
        (!std::strcmp(value, "true") || !std::strcmp(value, "1"));
}

uint64_t period(const State &state)
{
    const auto &m = state.drm.mode;
    if (m.clock && m.htotal && m.vtotal) {
        uint64_t ns = UINT64_C(1000000) * m.htotal * m.vtotal / m.clock;
        if (ns >= 1000000 && ns <= 1000000000) return ns;
    }
    return UINT64_C(1000000000) / (m.vrefresh ? m.vrefresh : 60);
}

void release(const State &state, Scanout &buffer)
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

void release(const State &state, ImportedScanout &frame)
{
    if (frame.map != MAP_FAILED) munmap(frame.map, frame.buffer.size);
    if (frame.fb) drmModeRmFB(state.drm.fd, frame.fb);
    if (frame.fd >= 0) close(frame.fd);
    frame = {};
}

int initialize(State &state)
{
    uint64_t monotonic=0;
    state.flip_clock_monotonic=!drmGetCap(state.drm.fd,DRM_CAP_TIMESTAMP_MONOTONIC,&monotonic) && monotonic==1;
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
    for (auto &buffer : state.buffers) release(state, buffer);
    if (state.saved) drmModeFreeCrtc(state.saved);
    state.saved = nullptr;
    close(state.drm.fd); state.drm.fd = -1;
    return FAIL;
}

void flipped(int, unsigned sequence, unsigned seconds, unsigned microseconds, unsigned crtc, void *data)
{
    auto &state = *static_cast<State *>(data);
    if (crtc && crtc != state.drm.crtc) {
        kms_log("AURORA_KMS_FLIP_WRONG_CRTC device=%u expected=%u actual=%u",
                     state.device, state.drm.crtc, crtc);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state.flip_mutex);
        state.event_ns = uint64_t(seconds) * 1000000000 + uint64_t(microseconds) * 1000;
        state.flip_sequence = sequence;
        state.flip_delivered = true;
    }
    state.flip_wake.notify_one();
}

bool FlipDispatcher::start(int drm_fd)
{
    stop_fd = eventfd(0, EFD_CLOEXEC);
    if (stop_fd < 0) return false;
    try {
        thread = std::thread([this, drm_fd] {
            pollfd fds[] = {{drm_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
            drmEventContext events = {};
            events.version = 3;
            events.page_flip_handler2 = flipped;
            for (;;) {
                int result = poll(fds, 2, -1);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0 || fds[1].revents || !(fds[0].revents & POLLIN)) break;
                if (drmHandleEvent(drm_fd, &events)) break;
            }
        });
    } catch (...) {
        close(stop_fd);
        stop_fd = -1;
        return false;
    }
    return true;
}

FlipDispatcher::~FlipDispatcher()
{
    if (stop_fd < 0) return;
    for (auto &output : outputs) {
        std::lock_guard<std::mutex> lock(output.vsync_mutex);
        output.stop = true;
        output.wake.notify_all();
    }
    for (auto &output : outputs) if (output.ticker.joinable()) output.ticker.join();
    uint64_t stop = 1;
    ssize_t result;
    do { result = write(stop_fd, &stop, sizeof(stop)); } while (result < 0 && errno == EINTR);
    thread.join();
    close(stop_fd);
}

int initialize_outputs()
{
    // Topology is stable for this service lifetime. Stock Composer receives
    // one initial hotplug notification for each output attached at startup.
    std::lock_guard<std::mutex> lock(topology_mutex);
    if (output_count) return OK;
    auto &primary = outputs[0];
    if (AuroraOpenDisplayDrm(&primary.drm, 1) || initialize(primary)) return FAIL;
    if (!dispatcher.start(primary.drm.fd)) {
        for (auto &buffer : primary.buffers) release(primary, buffer);
        if (primary.saved) drmModeFreeCrtc(primary.saved);
        primary.saved = nullptr;
        close(primary.drm.fd);
        primary.drm.fd = -1;
        return FAIL;
    }
    output_count = 1;
    if (!env_enabled("AURORA_MULTI_DISPLAY")) return OK;

    AuroraDrmDisplay extra[2];
    const int count = AuroraFindAdditionalDisplays(&primary.drm, extra);
    if (count < 0) kms_log("AURORA_KMS_ENUM_FAILED errno=%d", errno);
    for (int i = 0; i < count; ++i) {
        auto &output = outputs[output_count];
        output.drm = extra[i];
        output.drm.fd = fcntl(primary.drm.fd, F_DUPFD_CLOEXEC, 0);
        output.device = output_count;
        if (output.drm.fd >= 0 && !initialize(output)) {
            ++output_count;
        } else {
            kms_log("AURORA_KMS_OUTPUT_FAILED connector=%u crtc=%u errno=%d",
                         output.drm.connector, output.drm.crtc, errno);
        }
    }
    return OK;
}

State *find_output(uint32_t device)
{
    if (device >= outputs.size() || initialize_outputs() || device >= output_count) return nullptr;
    return &outputs[device];
}

bool fingerprint(ImportedScanout &frame, AuroraFrameSamples::Result &result);
void check_retired(State &state, ImportedScanout &frame);
int wait_flip(State &state)
{
    if (!state.pending) return OK;
    // The card event thread signals the output that submitted the flip. Each
    // Commit waits with its own lock, and never consumes another CRTC's event.
    uint64_t ns;
    uint32_t sequence;
    {
        std::unique_lock<std::mutex> lock(state.flip_mutex);
        if (!state.flip_wake.wait_for(lock, std::chrono::seconds(2), [&] { return state.flip_delivered; })) {
            kms_log("AURORA_KMS_FLIP_TIMEOUT device=%u connector=%u crtc=%u",
                         state.device, state.drm.connector, state.drm.crtc);
            errno = ETIMEDOUT;
            return FAIL;
        }
        state.flip_delivered = false;
        ns = state.event_ns;
        sequence = state.flip_sequence;
    }
    // Drop the scanout reservation only on the replacement's flip event.
    // FB registrations can outlive presentation and be reused by this pool.
    const auto *previous_import = state.imported_front;
    const uint32_t previous_fb = previous_import ? previous_import->fb :
        state.front >= 0 ? state.buffers[state.front].fb : 0;
    state.imported_front = state.imported_pending;
    state.imported_pending = nullptr;
    // The replaced buffer left the screen with this event and is still owned
    // here; HdiOutput releases it to the producer only after Commit returns.
    if (previous_import && previous_import != state.imported_front)
        check_retired(state, *const_cast<ImportedScanout *>(previous_import));
    state.front = state.pending_slot;
    state.pending = false;
    const uint64_t previous = state.last_flip_ns;
    state.last_flip_ns = ns;
    ++state.flips;
    if (state.imported_front && state.flips <= 16384 && aurora_display_trace_enabled()) {
        const auto &frame = *state.imported_front;
        aurora_display_trace("AURORA_DIRECT_BUFFER_FLIP device=%llu inode=%llu timestamp_ns=%llu monotonic=%d output=%u"
            " fb=%u previous_fb=%u previous_device=%llu previous_inode=%llu",
            static_cast<unsigned long long>(frame.dev), static_cast<unsigned long long>(frame.ino),
            static_cast<unsigned long long>(ns), state.flip_clock_monotonic, state.device, frame.fb, previous_fb,
            previous_import ? static_cast<unsigned long long>(previous_import->dev) : 0ULL,
            previous_import ? static_cast<unsigned long long>(previous_import->ino) : 0ULL);
    }
    if (state.scan_sample.valid) {
        const auto &sample = state.scan_sample;
        if (state.imported_front && state.imported_front->fb == sample.fb) {
            char regions[AuroraFrameSamples::regions * 4 + 1];
            AuroraFrameSamples::format_regions(sample.result, regions);
            aurora_display_trace("AURORA_SCAN_SAMPLE device=%u timestamp_ns=%llu fb=%u inode=%llu src=%d"
                " sampled_ns=%llu cost_us=%llu regions=%s", state.device, static_cast<unsigned long long>(ns),
                sample.fb, sample.inode, sample.result.serial, sample.sampled_ns, sample.cost_ns / 1000, regions);
        }
        state.scan_sample.valid = false;
    }
    static const bool full_trace = env_enabled("AURORA_KMS_FLIP_TRACE");
    if (full_trace || state.flips <= 3 || state.flips % 120 == 0) {
        kms_log("AURORA_KMS_FLIP device=%u connector=%u crtc=%u count=%llu sequence=%u timestamp_ns=%llu interval_ns=%llu monotonic=%d direct=%d",
                     state.device, state.drm.connector, state.drm.crtc,
                     static_cast<unsigned long long>(state.flips), sequence, static_cast<unsigned long long>(ns),
                     static_cast<unsigned long long>(previous && ns >= previous ? ns - previous : 0),
                     state.flip_clock_monotonic, state.imported_front != nullptr);
    }
    return OK;
}

int complete_flip(State &state, AuroraPerfTrace &trace)
{
    trace.mark("pageflip_submit");
    int status = wait_flip(state);
    trace.mark("pageflip_wait");
    trace.result(status);
    if (!status) trace.event(state.flip_clock_monotonic ? "drm_present_monotonic"
                                                       : "drm_present_clock_unspecified", state.last_flip_ns);
    return status;
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

ImportedScanout *import_client_framebuffer(State &state, bool boot_direct, AuroraPerfTrace &trace)
{
    static const bool enabled = [] {
        const char *value = std::getenv("AURORA_DIRECT_SCANOUT");
        return value && !std::strcmp(value, "1");
    }();
    const auto &client = state.client;
    const auto &mode = state.drm.mode;
    const auto &crop = state.crop;
    const bool full_crop = !crop.x && !crop.y &&
        ((!crop.w && !crop.h) || (crop.w == client.width && crop.h == client.height));
    if (!enabled || (!state.device && !state.boot_handoff.done() && !boot_direct) || state.front < 0) return 0;
    if (!full_crop || client.width != mode.hdisplay || client.height != mode.vdisplay) {
        trace.mark("direct_fallback_geometry");
        return 0;
    }
    if (AuroraFrameCapture::token()) {
        trace.mark("direct_fallback_capture");
        return 0;
    }

    // Interpret the existing opaque display target with the same RGB byte
    // ordering as the copy path. Import only the allocator's linear layout.
    struct stat identity {};
    if (fstat(state.client_fd, &identity)) return nullptr;
    for (auto &entry : state.imports) {
        if (entry.fb && entry.dev == identity.st_dev && entry.ino == identity.st_ino &&
            entry.buffer.format == client.format && entry.buffer.stride == client.stride &&
            entry.buffer.width == client.width && entry.buffer.height == client.height)
            return &entry;
    }
    const bool rgba = client.format == PIXEL_FMT_RGBA_8888 || client.format == PIXEL_FMT_RGBX_8888;
    const uint32_t format = rgba ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_XRGB8888;
    uint32_t handles[4] = {}, offsets[4] = {};
    // PRIME can return the same GEM handle on two imports on a shared card fd.
    std::lock_guard<std::mutex> lock(prime_mutex);
    if (drmPrimeFDToHandle(state.drm.fd, state.client_fd, &handles[0])) {
        trace.mark("direct_fallback_prime");
        return 0;
    }
    uint32_t pitches[4] = {static_cast<uint32_t>(client.stride)};
    uint32_t fb = 0;
    const int result = drmModeAddFB2(state.drm.fd, client.width, client.height, format,
                                    handles, pitches, offsets, &fb, 0);
    if (result) {
        const int failure = errno;
        if (!state.fb_reject_reported) {
            fprintf(stderr, "AURORA_DIRECT_FB_REJECT format=0x%x stride=%d errno=%d\n",
                    format, client.stride, failure);
            state.fb_reject_reported = true;
        }
        trace.mark("direct_fallback_addfb");
    }
    // Once installed, the framebuffer owns its own GEM reference.
    drm_gem_close release_handle = {handles[0], 0};
    ioctl(state.drm.fd, DRM_IOCTL_GEM_CLOSE, &release_handle);
    if (result) return nullptr;
    int fd = fcntl(state.client_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        drmModeRmFB(state.drm.fd, fb);
        return nullptr;
    }
    // At most one slot is being scanned and one is pending. Never evict
    // either on a timeout, or if a producer supplies a new pool mid-stream.
    for (;;) {
        auto &entry = state.imports[state.next_import];
        state.next_import = (state.next_import + 1) % state.imports.size();
        if (&entry == state.imported_front || &entry == state.imported_pending) continue;
        release(state, entry);
        entry = ImportedScanout{};
        entry.fb = fb; entry.fd = fd; entry.buffer = client;
        entry.dev = identity.st_dev; entry.ino = identity.st_ino;
        return &entry;
    }
}

bool find_atomic_primary_plane(State &state)
{
    if (state.tried_atomic_plane) return state.plane_fb_property != 0;
    state.tried_atomic_plane = true;
    const int fd = state.drm.fd;
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) return false;
    auto resources = drmModeGetPlaneResources(fd);
    if (!resources) return false;
    for (uint32_t i = 0; i < resources->count_planes && !state.plane_fb_property; ++i) {
        auto plane = drmModeGetPlane(fd, resources->planes[i]);
        if (!plane) continue;
        if (plane->possible_crtcs & (1U << state.drm.crtc_index)) {
            auto props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                bool primary = false;
                uint32_t fb_property = 0;
                for (uint32_t j = 0; j < props->count_props; ++j) {
                    auto prop = drmModeGetProperty(fd, props->props[j]);
                    if (!prop) continue;
                    if (!std::strcmp(prop->name, "type"))
                        primary = props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY;
                    else if (!std::strcmp(prop->name, "FB_ID"))
                        fb_property = prop->prop_id;
                    drmModeFreeProperty(prop);
                }
                if (primary && fb_property) {
                    state.primary_plane = plane->plane_id;
                    state.plane_fb_property = fb_property;
                }
                drmModeFreeObjectProperties(props);
            }
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(resources);
    if (auto props = drmModeObjectGetProperties(fd, state.drm.crtc, DRM_MODE_OBJECT_CRTC)) {
        for (uint32_t j = 0; j < props->count_props; ++j) {
            auto prop = drmModeGetProperty(fd, props->props[j]);
            if (!prop) continue;
            if (!std::strcmp(prop->name, "OUT_FENCE_PTR")) state.crtc_out_fence_property = prop->prop_id;
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
    }
    return state.plane_fb_property != 0;
}

// With out_fence, KMS returns a sync_file that signals when this FB is on
// screen, i.e. when the previously scanned FB has been released by KMS.
int atomic_flip(State &state, uint32_t fb, int *out_fence = nullptr)
{
    if (out_fence) *out_fence = -1;
    if (!find_atomic_primary_plane(state)) return FAIL;
    auto request = drmModeAtomicAlloc();
    if (!request) return FAIL;
    int32_t kernel_fence = -1;
    int result = drmModeAtomicAddProperty(request, state.primary_plane, state.plane_fb_property, fb);
    if (result >= 0 && out_fence && state.crtc_out_fence_property)
        result = drmModeAtomicAddProperty(request, state.drm.crtc, state.crtc_out_fence_property,
                                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&kernel_fence)));
    if (result >= 0)
        result = drmModeAtomicCommit(state.drm.fd, request,
                                     DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &state);
    drmModeAtomicFree(request);
    if (out_fence) {
        if (result >= 0) *out_fence = kernel_fence;
        else if (kernel_fence >= 0) close(kernel_fence);
    }
    return result;
}

struct CopySamples {
    uint32_t source = 2166136261u, destination = 2166136261u;
    unsigned nonblack = 0;
};

int copy_client(State &state, Scanout &output, const BufferHandle &client, int client_fd,
                Rect crop, bool inspect_boot, AuroraPerfTrace &perf, CopySamples *samples = nullptr)
{
    if (!crop.w && !crop.h) crop = {0, 0, client.width, client.height};
    if (crop.x < 0 || crop.y < 0 || crop.w <= 0 || crop.h <= 0 ||
        int64_t(crop.x) + crop.w > client.width || int64_t(crop.y) + crop.h > client.height) return PARAM;
    void *source = mmap(nullptr, client.size, PROT_READ, MAP_SHARED, client_fd, 0);
    if (source == MAP_FAILED) return FAIL;
    perf.mark("client_mmap");
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(client_fd, DMA_BUF_IOCTL_SYNC, &sync)) { munmap(source, client.size); return FAIL; }
    dma_buf_sync output_sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (ioctl(output.dma_fd, DMA_BUF_IOCTL_SYNC, &output_sync)) {
        const int saved_errno = errno;
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        (void)ioctl(client_fd, DMA_BUF_IOCTL_SYNC, &sync);
        munmap(source, client.size); errno = saved_errno; return FAIL;
    }
    perf.mark("dma_sync_start");
    bool rgba = client.format == PIXEL_FMT_RGBA_8888 || client.format == PIXEL_FMT_RGBX_8888;
    unsigned token = state.device ? 0 : AuroraFrameCapture::token();
    state.client_capture.save(token,"client",nullptr,static_cast<const uint8_t *>(source),
        client.width,client.height,client.stride,rgba,false);
    const unsigned frame = ++state.frame;
    bool trace = std::getenv("AURORA_GRAPHICS_TRACE") && (frame <= 5 || frame % 120 == 0);
    timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t mono_ms = uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    uint64_t nonblack=0;
    bool has_rgb=false;
    for (uint32_t y = 0; y < state.drm.mode.vdisplay; ++y) {
        auto src = static_cast<const uint8_t *>(source) +
            (crop.y + uint64_t(y) * crop.h / state.drm.mode.vdisplay) * client.stride;
        auto dst = static_cast<uint8_t *>(output.map) + size_t(y) * output.pitch;
        if (crop.w == state.drm.mode.hdisplay) {
            const auto row=src+size_t(crop.x)*4;
            AuroraCopyRow(dst,row,crop.w,rgba,true);
            if (inspect_boot && !has_rgb) for (int x=0;x<crop.w;++x)
                if (row[x*4] || row[x*4+1] || row[x*4+2]) { has_rgb=true; break; }
            if (trace) for (int x=0;x<crop.w;++x)
                if (row[x*4] || row[x*4+1] || row[x*4+2]) ++nonblack;
            continue;
        }
        for (uint32_t x = 0; x < state.drm.mode.hdisplay; ++x) {
            const auto pixel = src + (crop.x + uint64_t(x) * crop.w / state.drm.mode.hdisplay) * 4;
            dst[4*x] = pixel[rgba ? 2 : 0]; dst[4*x+1] = pixel[1];
            dst[4*x+2] = pixel[rgba ? 0 : 2]; dst[4*x+3] = 255;
            if (inspect_boot && (pixel[0]||pixel[1]||pixel[2])) has_rgb=true;
            if (trace && (pixel[0]||pixel[1]||pixel[2])) ++nonblack;
        }
    }
    perf.mark("cpu_pixel_copy");
    // Sample only the already-mapped preservation copy, inside its existing
    // DMA sync scope. Directly scanned buffers are never mapped for tracing.
    if (samples) {
        const unsigned width = state.drm.mode.hdisplay, height = state.drm.mode.vdisplay;
        for (unsigned row = 1; row <= 5; ++row) {
            const unsigned y = row * height / 6;
            const auto src = static_cast<const uint8_t *>(source) +
                (crop.y + uint64_t(y) * crop.h / height) * client.stride;
            const auto dst = static_cast<const uint8_t *>(output.map) + size_t(y) * output.pitch;
            for (unsigned col = 1; col <= 5; ++col) {
                const unsigned x = col * width / 6;
                const auto s = src + (crop.x + uint64_t(x) * crop.w / width) * 4;
                const auto d = dst + x * 4;
                const uint32_t from = uint32_t(s[rgba ? 2 : 0]) << 16 | uint32_t(s[1]) << 8 | s[rgba ? 0 : 2];
                const uint32_t to = uint32_t(d[0]) << 16 | uint32_t(d[1]) << 8 | d[2];
                samples->source = (samples->source ^ from) * 16777619u;
                samples->destination = (samples->destination ^ to) * 16777619u;
                if (from) ++samples->nonblack;
            }
        }
    }
    if (trace) {
        std::fprintf(stderr,"AURORA_COMPOSER_FRAME n=%u mono_ms=%llu source=%dx%d format=%d crop=%d,%d,%d,%d nonblack=%llu\n",
            frame, static_cast<unsigned long long>(mono_ms), client.width,client.height,client.format,crop.x,crop.y,crop.w,crop.h,
            static_cast<unsigned long long>(nonblack));
    }
    state.scanout_capture.save(token,"scanout",nullptr,static_cast<const uint8_t *>(output.map),
        state.drm.mode.hdisplay,state.drm.mode.vdisplay,output.pitch,false,false);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    int result = ioctl(client_fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(source, client.size);
    // Publish CPU writes through the exporter's dma-buf cache contract before
    // handing this reused GEM buffer to KMS. msync is not supported by every
    // DRM mmap implementation and does not express DMA ownership transitions.
    output_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    int output_result = ioctl(output.dma_fd, DMA_BUF_IOCTL_SYNC, &output_sync);
    perf.mark("dma_sync_end_unmap");
    if (inspect_boot) {
        const bool held = state.boot_handoff.hold(has_rgb) && state.front >= 0;
        if (held != state.handoff_held_frame) std::fprintf(stderr,
            "AURORA_BOOT_HANDOFF held=%d mono_ms=%llu rgb=%d done=%d\n", held,
            static_cast<unsigned long long>(mono_ms), has_rgb, state.boot_handoff.done());
        state.handoff_held_frame = held;
    }
    return result || output_result ? FAIL : OK;
}

int detach_imported_framebuffer(State &state, const char *reason)
{
    if (wait_flip(state)) return FAIL;
    if (!state.imported_front) return OK;
    const auto &front = *state.imported_front;

    // The producer may recycle its buffer after Clear succeeds. Preserve the
    // frame actually on screen, which can differ from a queued client target.
    // Only retire the import after KMS confirms the owned replacement.
    AuroraPerfTrace trace("composer-clear");
    trace.object(&front, front.buffer.width, front.buffer.height);
    const int slot = state.front == 0 ? 1 : 0;
    auto &replacement = state.buffers[slot];
    const bool log_copy = aurora_display_trace_enabled();
    const auto begin = log_copy ? aurora_display_time_ns() : 0;
    CopySamples samples;
    if (copy_client(state, replacement, front.buffer, front.fd, {}, false, trace, log_copy ? &samples : nullptr)) {
        kms_log("AURORA_KMS_CLEAR_COPY_FAILED device=%u fb=%u errno=%d", state.device, front.fb, errno);
        return FAIL;
    }
    const auto copied = log_copy ? aurora_display_time_ns() : 0;
    if (atomic_flip(state, replacement.fb)) {
        kms_log("AURORA_KMS_CLEAR_FLIP_FAILED device=%u fb=%u errno=%d", state.device, replacement.fb, errno);
        return FAIL;
    }
    const uint32_t previous_fb = front.fb;
    state.pending_slot = slot;
    state.pending = true;
    int result = complete_flip(state, trace);
    if (!result) {
        kms_log("AURORA_KMS_CLEAR_PRESERVED device=%u imported=%u replacement=%u", state.device, previous_fb, replacement.fb);
        if (log_copy)
            aurora_display_trace("AURORA_KMS_CLEAR_COPY device=%u reason=%s imported=%u replacement=%u source_device=%llu source_inode=%llu"
                " begin_ns=%llu copied_ns=%llu timestamp_ns=%llu monotonic=%d source_rgb=%08x copy_rgb=%08x nonblack_samples=%u",
                state.device, reason, previous_fb, replacement.fb,
                static_cast<unsigned long long>(front.dev), static_cast<unsigned long long>(front.ino), begin, copied,
                static_cast<unsigned long long>(state.last_flip_ns), state.flip_clock_monotonic,
                samples.source, samples.destination, samples.nonblack);
    }
    return result;
}

bool frame_samples_enabled()
{
    static const bool enabled = aurora_display_trace_enabled() && !env_enabled("AURORA_FRAME_SAMPLES_OFF");
    return enabled;
}

bool fingerprint(ImportedScanout &frame, AuroraFrameSamples::Result &result)
{
    const auto &buffer = frame.buffer;
    if (frame.fd < 0 || buffer.width <= 0 || buffer.height <= 0 ||
        int64_t(buffer.stride) * buffer.height > buffer.size) return false;
    if (frame.map == MAP_FAILED) {
        frame.map = mmap(nullptr, buffer.size, PROT_READ, MAP_SHARED, frame.fd, 0);
        if (frame.map == MAP_FAILED) return false;
    }
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(frame.fd, DMA_BUF_IOCTL_SYNC, &sync)) return false;
    result = AuroraFrameSamples::sample(static_cast<const uint8_t *>(frame.map), buffer.width, buffer.height,
                                        buffer.stride, buffer.format);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)ioctl(frame.fd, DMA_BUF_IOCTL_SYNC, &sync);
    return true;
}

// Fingerprint a direct frame while its flip is pending. The acquire fence was
// waited in SetDisplayClientBuffer, so rendering is complete; reads never
// modify the scanned buffer. Bounded to the first 20000 flips per output.
void sample_scanout(State &state, ImportedScanout &frame)
{
    frame.sampled = false;
    if (!frame_samples_enabled() || state.scan_samples >= 20000) return;
    const auto begin = aurora_display_time_ns();
    auto &sample = state.scan_sample;
    if (!fingerprint(frame, sample.result)) return;
    frame.shown = sample.result;
    frame.sampled = true;
    sample.fb = frame.fb;
    sample.inode = static_cast<unsigned long long>(frame.ino);
    sample.sampled_ns = begin;
    sample.cost_ns = aurora_display_time_ns() - begin;
    sample.valid = true;
    ++state.scan_samples;
}

// A buffer must not change while it is on screen. Compare its contents as it
// leaves the screen with the fingerprint taken when it was flipped on.
void check_retired(State &state, ImportedScanout &frame)
{
    if (!frame.sampled) return;
    frame.sampled = false;
    // An asynchronously returned flip lets the producer reclaim the previous
    // FB as soon as the out-fence signals, before this check would run.
    if (state.async_flips) return;
    AuroraFrameSamples::Result now;
    if (!fingerprint(frame, now)) return;
    unsigned changed = 0;
    for (int i = 0; i < AuroraFrameSamples::regions; ++i) changed += now.region[i] != frame.shown.region[i];
    ++state.retire_checked;
    if (changed) {
        ++state.retire_changed;
        if (state.retire_changed <= 200) {
            char before[AuroraFrameSamples::regions * 4 + 1], after[AuroraFrameSamples::regions * 4 + 1];
            AuroraFrameSamples::format_regions(frame.shown, before);
            AuroraFrameSamples::format_regions(now, after);
            aurora_display_trace("AURORA_SCAN_RETIRE_CHANGED device=%u fb=%u inode=%llu changed=%u src_shown=%d src_now=%d"
                " at_ns=%llu", state.device, frame.fb, static_cast<unsigned long long>(frame.ino), changed,
                frame.shown.serial, now.serial, aurora_display_time_ns());
            aurora_display_trace("AURORA_SCAN_RETIRE_REGIONS fb=%u before=%s", frame.fb, before);
            aurora_display_trace("AURORA_SCAN_RETIRE_REGIONS fb=%u after=%s", frame.fb, after);
        }
    }
    if (state.retire_checked % 600 == 0)
        aurora_display_trace("AURORA_SCAN_RETIRE_SUMMARY device=%u checked=%u changed=%u", state.device,
                             state.retire_checked, state.retire_changed);
}

void ticker(State &state)
{
    std::unique_lock<std::mutex> lock(state.vsync_mutex);
    unsigned sequence = 0;
    while (!state.stop) {
        state.wake.wait(lock, [&] { return state.stop || (state.enabled && state.vblank); });
        if (state.stop) break;
        const auto generation = state.vsync_generation;
        const auto interval = std::chrono::nanoseconds(state.vsync_period_ns);
        auto next = std::chrono::steady_clock::now() + interval;
        while (!state.wake.wait_until(lock, next, [&, generation] {
            return state.stop || generation != state.vsync_generation;
        })) {
            auto callback = state.vblank; auto data = state.vblank_data;
            timespec timestamp; clock_gettime(CLOCK_MONOTONIC, &timestamp);
            uint64_t ns = uint64_t(timestamp.tv_sec) * 1000000000 + timestamp.tv_nsec;
            const auto now = std::chrono::steady_clock::now();
            const auto ticks = 1 + (now - next) / interval;
            sequence += static_cast<unsigned>(ticks);
            next += ticks * interval;
            lock.unlock(); callback(sequence, ns, data); lock.lock();
        }
    }
}

State::~State()
{
    if (drm.fd >= 0) {
        if (saved) {
            if (front >= 0) drmModeSetCrtc(drm.fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                          saved->mode_valid ? &drm.connector : nullptr, saved->mode_valid ? 1 : 0,
                          saved->mode_valid ? &saved->mode : nullptr);
            drmModeFreeCrtc(saved);
        }
        for (auto &entry : imports) release(*this, entry);
        for (auto &buffer : buffers) release(*this, buffer);
        close(drm.fd);
    }
    if (client_fd >= 0) close(client_fd);
}
bool layer(const State &state, uint32_t id) { return state.layers.count(id); }
}

#define API extern "C" __attribute__((visibility("default"))) int32_t
API RegHotPlugCallback(Hotplug callback, void *data)
{
    fprintf(stderr, "AURORA_COMPOSER_HOTPLUG_REGISTER callback=%p\n", reinterpret_cast<void *>(callback));
    if (!callback) return PARAM;
    if (initialize_outputs()) return FAIL;
    for (unsigned device = 0; device < output_count; ++device) {
        const auto &output = outputs[device];
        const auto &drm = output.drm;
        kms_log("AURORA_KMS_OUTPUT device=%u driver=%s connector=%u type=%u type_id=%u crtc=%u mode=%ux%u period_ns=%llu",
                     device, drm.driver, drm.connector, drm.connector_type, drm.connector_type_id, drm.crtc,
                     drm.mode.hdisplay, drm.mode.vdisplay, static_cast<unsigned long long>(period(output)));
        callback(device, true, data);
        fprintf(stderr, "AURORA_COMPOSER_HOTPLUG_SENT device=%u connected=1\n", device);
    }
    return OK;
}
API GetDisplayCapability(uint32_t device, Capability *out)
{
    fprintf(stderr, "AURORA_COMPOSER_CAPABILITY device=%u\n", device);
    const auto *output = find_output(device);
    if (!output || !out) return PARAM;
    const auto &state = *output;
    out->name = device ? "Aurora external display" : "Aurora internal display";
    // RenderService recognizes the MIPI enum as its built-in screen role.
    out->type = device ? DISP_INTF_HDMI : (output_count > 1 ? DISP_INTF_MIPI : DISP_INTF_LCD);
    out->phyWidth = state.drm.mm_width; out->phyHeight = state.drm.mm_height;
    out->supportLayers = 0; out->virtualDispCount = 0; out->supportWriteBack = false;
    out->propertyCount = 0; out->props.clear(); return OK;
}
API GetDisplaySupportedModes(uint32_t device, std::vector<Mode> *out)
{
    fprintf(stderr, "AURORA_COMPOSER_MODES device=%u\n", device);
    const auto *output = find_output(device);
    if (!output || !out) return PARAM;
    const auto &state = *output;
    *out = {{state.drm.mode.hdisplay, state.drm.mode.vdisplay,
             static_cast<uint32_t>((1000000000ULL + period(state)/2) / period(state)), 0}};
    return OK;
}
API GetDisplayIdentificationData(uint32_t device, uint8_t &port, std::vector<uint8_t> &edid)
{
    const auto *output = find_output(device);
    if (!output) return PARAM;
    const auto &drm = output->drm;
    auto *properties = drmModeObjectGetProperties(drm.fd, drm.connector, DRM_MODE_OBJECT_CONNECTOR);
    if (!properties) return FAIL;
    uint32_t blob_id = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        auto *property = drmModeGetProperty(drm.fd, properties->props[i]);
        if (!property) continue;
        const bool matches = (property->flags & DRM_MODE_PROP_BLOB) && !std::strcmp(property->name, "EDID");
        drmModeFreeProperty(property);
        if (!matches) continue;
        blob_id = static_cast<uint32_t>(properties->prop_values[i]);
        break;
    }
    drmModeFreeObjectProperties(properties);
    if (!blob_id) return UNSUPPORTED;
    auto *blob = drmModeGetPropertyBlob(drm.fd, blob_id);
    if (!blob) return FAIL;
    const auto *bytes = static_cast<const uint8_t *>(blob->data);
    if (!bytes || !blob->length || blob->length % 128 || blob->length > 128 * 256) {
        drmModeFreePropertyBlob(blob);
        return FAIL;
    }
    edid.assign(bytes, bytes + blob->length);
    drmModeFreePropertyBlob(blob);
    port = static_cast<uint8_t>(device);
    return OK;
}
API GetDisplayMode(uint32_t device, uint32_t *mode) { if (!find_output(device) || !mode) return PARAM; *mode = 0; return OK; }
API SetDisplayMode(uint32_t device, uint32_t mode) { return !find_output(device) || mode ? PARAM : OK; }
API GetDisplayPowerStatus(uint32_t device, int32_t *status)
{
    auto *output = find_output(device);
    if (!output || !status) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.mutex);
    uint32_t power;
    if (AuroraGetDisplayPower(&state.drm, &power)) return errno == EOPNOTSUPP ? UNSUPPORTED : FAIL;
    *status = static_cast<int32_t>(power);
    return OK;
}
API SetDisplayPowerStatus(uint32_t device, int32_t status)
{
    auto *output = find_output(device);
    if (!output || status < 0 || status > 3) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (wait_flip(state)) return FAIL;
    if (!device && status != POWER_STATUS_ON) state.boot_handoff.stop();
    if (AuroraSetDisplayPower(&state.drm, static_cast<uint32_t>(status)))
        return errno == EOPNOTSUPP ? UNSUPPORTED : FAIL;
    return OK;
}
API GetDisplayBacklight(uint32_t device, uint32_t *level)
{
    if (device || !level) return PARAM;
    return AuroraGetBacklight(level) ? FAIL : OK;
}
API SetDisplayBacklight(uint32_t device, uint32_t level)
{
    if (device || level > 255) return PARAM;
    return AuroraSetBacklight(level) ? FAIL : OK;
}
API GetDisplayProperty(uint32_t, uint32_t, uint64_t *) { return UNSUPPORTED; }
API SetDisplayProperty(uint32_t, uint32_t, uint64_t) { return UNSUPPORTED; }
API GetDisplaySupportedColorGamuts(uint32_t device, std::vector<int32_t> *out)
{ if (!find_output(device) || !out) return PARAM; *out = {4}; return OK; }
API CreateLayer(uint32_t device, const AuroraComposer::LayerInfo *info, uint32_t *id)
{
    auto *output = find_output(device);
    if (!output || !info || !id || info->width <= 0 || info->height <= 0) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.layers.size() >= 128) return FAIL;
    *id = state.next_layer++; state.layers[*id] = -1; return OK;
}
API DestroyLayer(uint32_t device, uint32_t id)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    std::lock_guard<std::mutex> lock(output->mutex);
    return output->layers.erase(id) ? OK : PARAM;
}
API PrepareDisplayLayers(uint32_t device, bool *flush) { if (!find_output(device) || !flush) return PARAM; *flush = true; return OK; }
API GetDisplayCompChange(uint32_t device, std::vector<uint32_t> *ids, std::vector<int32_t> *types)
{
    auto *output = find_output(device);
    if (!output || !ids || !types) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.mutex);
    ids->clear(); types->clear();
    for (auto &item : state.layers) if (item.second != 0) {
        ids->push_back(item.first); types->push_back(0); item.second = 0;
    }
    return OK;
}
API SetLayerCompositionType(uint32_t device, uint32_t id, int32_t type)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    std::lock_guard<std::mutex> lock(output->mutex);
    if (!layer(*output, id)) return PARAM;
    output->layers[id] = type;
    return OK;
}
API SetDisplayClientBuffer(uint32_t device, const BufferHandle *buffer, int32_t fence)
{
    AuroraPerfTrace trace("composer-client");
    auto *output = find_output(device);
    if (!output || !buffer || !valid(*buffer)) return PARAM;
    auto &state = *output;
    trace.object(buffer,buffer->width,buffer->height);
    if (wait_fence(fence)) return FAIL;
    trace.mark("acquire_fence");
    int fd = fcntl(buffer->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return FAIL;
    std::lock_guard<std::mutex> lock(state.mutex);
    trace.mark("dup_and_lock");
    if (state.client_fd >= 0) close(state.client_fd);
    state.client_fd = fd; state.client = *buffer; trace.result(0); return OK;
}
API SetDisplayClientCrop(uint32_t device, const Rect *crop)
{
    auto *output = find_output(device);
    if (!output || !crop || crop->x < 0 || crop->y < 0 || crop->w <= 0 || crop->h <= 0) return PARAM;
    std::lock_guard<std::mutex> lock(output->mutex);
    output->crop = *crop;
    return OK;
}
API SetDisplayClientDamage(uint32_t device, const std::vector<Rect> *damage)
{ return !find_output(device) || !damage ? PARAM : OK; }
API ClearDisplayClientBuffer(uint32_t device)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.mutex);
    // RenderService clears its client cache whenever it trims memory. Keep the
    // scanned import on screen: its KMS framebuffer and duplicated DMA-BUF keep
    // the storage alive, and the producer only renders into buffers it owns
    // after the replacement flip has released them. A full CPU copy here
    // stalled every commit for 35-105 ms. AURORA_KMS_CLEAR_COPY=1 restores it.
    static const bool copy_on_clear = env_enabled("AURORA_KMS_CLEAR_COPY");
    if (copy_on_clear) {
        if ((state.imported_front || state.imported_pending) && detach_imported_framebuffer(state, "client_clear"))
            return FAIL;
    } else if (wait_flip(state)) {
        return FAIL;
    }
    unsigned released = 0;
    for (auto &entry : state.imports) {
        if (&entry == state.imported_front || !entry.fb) continue;
        release(state, entry);
        ++released;
    }
    if (state.imported_front)
        kms_log("AURORA_KMS_CLEAR_RETAINED device=%u fb=%u inode=%llu released=%u", state.device,
                state.imported_front->fb, static_cast<unsigned long long>(state.imported_front->ino), released);
    if (state.client_fd >= 0) close(state.client_fd);
    state.client_fd = -1;
    state.crop = {};
    return OK;
}
API Commit(uint32_t device, int32_t *fence)
{
    AuroraPerfTrace trace("composer-commit");
    auto *output = find_output(device);
    if (!output || !fence) return PARAM;
    auto &state = *output;
    *fence = -1;
    std::lock_guard<std::mutex> lock(state.mutex);
    trace.mark("lock");
    if (state.client_fd < 0) return FAIL;
    trace.object(&state.client,state.client.width,state.client.height);
    trace.mark("initialize");
    if (wait_flip(state)) return FAIL;
    trace.mark("previous_flip_wait");
    bool inspect_boot = false;
    if (!device && !state.boot_handoff.done()) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto mono_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        inspect_boot = state.boot_handoff.inspect(mono_ms,
            boot_parameter("const.aurora.bootanimation.handoff"),
            boot_parameter("bootevent.bootanimation.ready"), boot_parameter("bootevent.boot.completed"));
    }
    const bool boot_direct = inspect_boot && state.boot_handoff.playing(
        boot_parameter("bootevent.bootanimation.started"), boot_parameter("bootevent.bootanimation.finished"));
    if (auto *imported = import_client_framebuffer(state, boot_direct, trace)) {
        trace.mark("direct_import");
        const uint32_t fb = imported->fb;
        // Boot handoff frames keep the synchronous policy. Normal frames
        // return KMS's out-fence as the present fence: HdiOutput releases the
        // previous FB with it, so the producer waits for the real flip while
        // RenderService's hardware thread continues with the next frame.
        static const bool async_enabled = !env_enabled("AURORA_KMS_SYNC_COMMIT");
        const bool async = async_enabled && !inspect_boot && !boot_direct;
        int present_fence = -1;
        if (!atomic_flip(state, fb, async ? &present_fence : nullptr)) {
            state.imported_pending = imported;
            state.pending_slot = state.front;
            state.pending = true;
            // Use the flip wait to fingerprint the frame KMS is about to show.
            sample_scanout(state, *imported);
            trace.mark("scan_sample");
            if (present_fence >= 0) {
                if (!state.async_flips++)
                    kms_log("AURORA_KMS_ASYNC_COMMIT device=%u out_fence=1", device);
                *fence = present_fence;
                trace.mark("async_present_fence");
                trace.result(0);
                return OK;
            }
            // The stock HdiOutput holds the current client until the next
            // commit, then releases the previous one with our commit fence.
            // Synchronous completion makes that previous client reusable.
            const int status = complete_flip(state, trace);
            if (!status && !state.direct_reported) {
                fprintf(stderr, "AURORA_DIRECT_SCANOUT_PRESENTED output=%u\n", device);
                state.direct_reported = true;
            }
            if (!status && boot_direct && !state.boot_direct_reported) {
                kms_log("AURORA_BOOT_ANIMATION_DIRECT device=%u fb=%u", device, fb);
                state.boot_direct_reported = true;
            }
            return status;
        }
        if (imported != state.imported_front) release(state, *imported);
        trace.mark("direct_fallback_atomic");
    }
    int slot = state.front == 0 ? 1 : 0;
    if (!inspect_boot) state.handoff_held_frame = false;
    int result = copy_client(state, state.buffers[slot], state.client, state.client_fd, state.crop,
                             inspect_boot, trace);
    if (result) return result;
    // Client/output DMA ownership has been returned already. Retaining the
    // previous KMS front buffer is bounded, nonblocking and boot-only.
    if (state.handoff_held_frame) {
        trace.mark("boot_hold");
        // A successful hold allows the client to release buffers. Own a copy
        // of the visible image before returning if the last frame was direct.
        result = detach_imported_framebuffer(state, "boot_handoff");
        trace.result(result);
        return result;
    }
    if (state.front < 0) {
        if (drmModeSetCrtc(state.drm.fd, state.drm.crtc, state.buffers[slot].fb, 0, 0,
                           &state.drm.connector, 1, &state.drm.mode)) {
            kms_log("AURORA_KMS_MODESET_FAILED device=%u connector=%u crtc=%u errno=%d",
                         device, state.drm.connector, state.drm.crtc, errno);
            return FAIL;
        }
        kms_log("AURORA_KMS_MODESET device=%u connector=%u crtc=%u fb=%u mode=%ux%u period_ns=%llu",
                     device, state.drm.connector, state.drm.crtc, state.buffers[slot].fb,
                     state.drm.mode.hdisplay, state.drm.mode.vdisplay,
                     static_cast<unsigned long long>(period(state)));
        state.front = slot;trace.mark("first_modeset");trace.result(0);return OK;
    }
    state.pending_slot = slot;
    const uint32_t fb = state.buffers[slot].fb;
    const int flip_result = state.imported_front ? atomic_flip(state, fb)
        : drmModePageFlip(state.drm.fd, state.drm.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, &state);
    if (flip_result) {
        kms_log("AURORA_KMS_FLIP_FAILED device=%u connector=%u crtc=%u errno=%d",
                     device, state.drm.connector, state.drm.crtc, errno);
        return FAIL;
    }
    state.pending = true;
    return complete_flip(state, trace);
}
API GetDisplayReleaseFence(uint32_t device, std::vector<uint32_t> *ids, std::vector<int32_t> *fences)
{ if (!find_output(device) || !ids || !fences) return PARAM; ids->clear(); fences->clear(); return OK; }
API RegDisplayVBlankCallback(uint32_t device, Vblank callback, void *data)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.vsync_mutex);
    state.vsync_period_ns = period(state);
    state.vblank = callback; state.vblank_data = data;
    ++state.vsync_generation;
    if (!state.ticker.joinable()) state.ticker = std::thread(ticker, std::ref(state));
    state.wake.notify_all(); return OK;
}
API SetDisplayVsyncEnabled(uint32_t device, bool enabled)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    auto &state = *output;
    std::lock_guard<std::mutex> lock(state.vsync_mutex);
    if (state.enabled != enabled) {
        state.enabled = enabled;
        ++state.vsync_generation;
        state.wake.notify_all();
    }
    return OK;
}
API GetDisplayVBlankPeriod(uint32_t device, uint64_t *out)
{
    const auto *output = find_output(device);
    if (!output || !out) return PARAM;
    *out = period(*output);
    return OK;
}

// These describe CLIENT layers. RenderService applies them during composition;
// this backend does not advertise any hardware layer or cursor capability.
#define LAYER_SETTER(name, type) \
API name(uint32_t device, uint32_t id, type) { \
    auto *output = find_output(device); \
    if (!output) return PARAM; \
    std::lock_guard<std::mutex> lock(output->mutex); return layer(*output, id) ? OK : PARAM; }
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
{
    auto *output = find_output(device);
    if (!output || !buffer) return PARAM;
    std::lock_guard<std::mutex> lock(output->mutex);
    return layer(*output, id) ? OK : PARAM;
}
API ClearLayerBuffer(uint32_t device, uint32_t id)
{
    auto *output = find_output(device);
    if (!output) return PARAM;
    std::lock_guard<std::mutex> lock(output->mutex);
    return layer(*output, id) ? OK : PARAM;
}
// Optional extension entry points must exist for the vendor adapter loader.
// Unknown signatures are intentionally never inspected or claimed supported.
#define OPTIONAL(name) API name(...) { return UNSUPPORTED; }
OPTIONAL(CreateVirtualDisplay) OPTIONAL(DestroyVirtualDisplay) OPTIONAL(SetVirtualDisplayBuffer)
OPTIONAL(RegSeamlessChangeCallback) OPTIONAL(GetDisplaySupportedModesExt) OPTIONAL(SetDisplayModeAsync)
OPTIONAL(SetLayerPerFrameParameter) OPTIONAL(SetDisplayPerFrameParameter) OPTIONAL(GetSupportedLayerPerFrameParameterKey)
OPTIONAL(RegRefreshCallback) OPTIONAL(GetHDRCapabilityInfos) OPTIONAL(RegDisplayVBlankIdleCallback)
OPTIONAL(EnableHardwareCursorStats) OPTIONAL(GetHardwareCursorStats) OPTIONAL(FastPresent)
OPTIONAL(RegHwcEventCallback) OPTIONAL(GetDumpInfo)
OPTIONAL(UpdateHardwareCursor) OPTIONAL(SetDisplayConstraint) OPTIONAL(SetDisplayActiveRegion)
OPTIONAL(SetDisplayOverlayResolution) OPTIONAL(UpdateConfig)
OPTIONAL(GetSupportLayerType) OPTIONAL(CommitTunnelLayer) OPTIONAL(SetTunnelLayerBuffer)
OPTIONAL(SetTunnelLayerId) OPTIONAL(SetTunnelLayerPosition) OPTIONAL(SetTunnelLayerProperty)
API ClearClientBuffer(uint32_t device) { return ClearDisplayClientBuffer(device); }
API InitDisplay() { return initialize_outputs(); }
// Full teardown occurs at library unload, after the service releases callbacks.
API DeinitDisplay() { return UNSUPPORTED; }
