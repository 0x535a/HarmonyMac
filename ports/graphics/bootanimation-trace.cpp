/* SPDX-License-Identifier: GPL-3.0-only
 * Bounded, process-local diagnostics for the original OHOS bootanimation.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace {
thread_local const char *last_egl_failure = "none";
std::atomic<FILE *> archive_file{nullptr};
std::atomic<bool> panel_archive{false};
std::atomic<PFNEGLGETPLATFORMDISPLAYEXTPROC> platform_display{nullptr};
constexpr unsigned frame_limit = 1024;

bool trace_frame(unsigned frame)
{
    return frame < 8 || (frame < frame_limit && frame % 8 == 0);
}

int trace_fd()
{
    static const int descriptor = [] {
        const int saved = errno;
        char executable[128]{};
        const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        const char *file = std::getenv("AURORA_BOOTANIMATION_TRACE_FILE");
        const int opened = length > 0 && !std::strcmp(executable, "/system/bin/bootanimation") && file
            ? open(file, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC) : -1;
        errno = saved;
        return opened;
    }();
    return descriptor;
}

bool logging_enabled() { return trace_fd() >= 0; }

void record(const char *stage, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

void record(const char *stage, const char *format, ...)
{
    if (!logging_enabled()) return;
    const int saved = errno;
    char message[512];
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    const int prefix = std::snprintf(message, sizeof(message),
        "mono=%lld.%09ld pid=%ld tid=%ld stage=%s ",
        static_cast<long long>(now.tv_sec), now.tv_nsec,
        static_cast<long>(getpid()), syscall(SYS_gettid), stage);
    if (prefix > 0 && static_cast<size_t>(prefix) < sizeof(message) - 2) {
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(message + prefix, sizeof(message) - prefix, format, args);
        va_end(args);
        if (written >= 0 && static_cast<size_t>(written) < sizeof(message) - prefix - 1) {
            const size_t end = prefix + written;
            message[end] = '\n';
            ssize_t result;
            do { result = write(trace_fd(), message, end + 1); } while (result < 0 && errno == EINTR);
        }
    }
    errno = saved;
}

__attribute__((constructor)) void announce_trace()
{
    record("trace", "event=loaded");
}

template <typename F> F next(const char *name)
{
    const int saved = errno;
    const auto fn = reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
    if (!fn) record(name, "event=symbol-missing");
    errno = saved;
    return fn;
}

template <typename T, typename F, typename... Args>
T trace_egl(std::atomic<unsigned> &counter, const char *stage, T failure, F fn, Args... args)
{
    const unsigned call = counter.fetch_add(1, std::memory_order_relaxed);
    const bool swap = !std::strcmp(stage, "eglSwapBuffers");
    const bool sample = swap ? call < frame_limit : call < 4;
    if (sample) record(stage, "event=begin call=%u", call);
    const T result = fn ? fn(args...) : failure;
    if (result == failure) last_egl_failure = stage;
    if (sample || (result == failure && call < frame_limit))
        record(stage, "event=end call=%u success=%d", call, result != failure);
    return result;
}

EGLDisplay traced_platform_display(EGLenum platform, void *display, const EGLint *attrs)
{
    static std::atomic<unsigned> calls{0};
    return trace_egl(calls, "eglGetPlatformDisplayEXT", EGL_NO_DISPLAY,
                     platform_display.load(), platform, display, attrs);
}

bool is_archive(const char *path)
{
    const char *ext = path ? std::strrchr(path, '.') : nullptr;
    return ext && !std::strcmp(ext, ".zip");
}
}

extern "C" FILE *fopen(const char *path, const char *mode)
{
    static auto original = next<decltype(&fopen)>("fopen");
    FILE *file = original ? original(path, mode) : nullptr;
    if (file && path && !std::strcmp(path, "/sys_prod/etc/bootanimation/aurora_bootpic_j514.zip"))
        panel_archive.store(true, std::memory_order_relaxed);
    if (logging_enabled() && is_archive(path)) {
        archive_file.store(file, std::memory_order_relaxed);
        record("resource.open", "path=%s success=%d errno=%d", path, file != nullptr, file ? 0 : errno);
    }
    return file;
}

extern "C" size_t fread(void *data, size_t item_size, size_t count, FILE *file)
{
    static auto original = next<decltype(&fread)>("fread");
    const size_t read = original ? original(data, item_size, count, file) : 0;
    if (file && file == archive_file.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> calls{0};
        const int saved = errno;
        const bool stream_error = read != count && ferror(file);
        errno = saved;
        if (calls.fetch_add(1, std::memory_order_relaxed) < 12 || stream_error)
            record("resource.read", "item_bytes=%zu requested=%zu actual=%zu errno=%d",
                   item_size, count, read, stream_error ? saved : 0);
    }
    return read;
}

extern "C" int fclose(FILE *file)
{
    static auto original = next<decltype(&fclose)>("fclose");
    FILE *expected = file;
    const bool archive = file && archive_file.compare_exchange_strong(expected, nullptr);
    const int result = original ? original(file) : EOF;
    if (archive) record("resource.close", "result=%d errno=%d", result, result ? errno : 0);
    return result;
}

#define TRACE_EGL(ret, name, failure, params, ...) \
    extern "C" EGLAPI ret EGLAPIENTRY name params { \
        static auto fn = next<decltype(&name)>(#name); \
        static std::atomic<unsigned> calls{0}; \
        return trace_egl(calls, #name, failure, fn, __VA_ARGS__); \
    }
TRACE_EGL(EGLDisplay, eglGetDisplay, EGL_NO_DISPLAY, (EGLNativeDisplayType display), display)
TRACE_EGL(EGLBoolean, eglInitialize, EGLBoolean(EGL_FALSE),
          (EGLDisplay d, EGLint *major, EGLint *minor), d, major, minor)
TRACE_EGL(EGLBoolean, eglBindAPI, EGLBoolean(EGL_FALSE), (EGLenum api), api)
TRACE_EGL(EGLBoolean, eglChooseConfig, EGLBoolean(EGL_FALSE),
          (EGLDisplay d, const EGLint *attrs, EGLConfig *cfg, EGLint size, EGLint *count),
          d, attrs, cfg, size, count)
TRACE_EGL(EGLContext, eglCreateContext, EGL_NO_CONTEXT,
          (EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attrs), d, cfg, share, attrs)
TRACE_EGL(EGLSurface, eglCreatePbufferSurface, EGL_NO_SURFACE,
          (EGLDisplay d, EGLConfig cfg, const EGLint *attrs), d, cfg, attrs)
TRACE_EGL(EGLSurface, eglCreateWindowSurface, EGL_NO_SURFACE,
          (EGLDisplay d, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attrs), d, cfg, win, attrs)
TRACE_EGL(EGLBoolean, eglMakeCurrent, EGLBoolean(EGL_FALSE),
          (EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx), d, draw, read, ctx)
TRACE_EGL(EGLBoolean, eglSwapBuffers, EGLBoolean(EGL_FALSE), (EGLDisplay d, EGLSurface surface), d, surface)
#undef TRACE_EGL

extern "C" EGLAPI EGLint EGLAPIENTRY eglGetError()
{
    static auto original = next<decltype(&eglGetError)>("eglGetError");
    static std::atomic<unsigned> reports{0};
    const EGLint result = original ? original() : EGL_NOT_INITIALIZED;
    if (result != EGL_SUCCESS && reports.fetch_add(1, std::memory_order_relaxed) < 16)
        record("eglGetError", "after=%s code=0x%04x", last_egl_failure, result);
    last_egl_failure = "none";
    return result;
}

extern "C" EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name)
{
    static auto original = next<decltype(&eglGetProcAddress)>("eglGetProcAddress");
    auto result = original ? original(name) : nullptr;
    if (logging_enabled() && name && !std::strcmp(name, "eglGetPlatformDisplayEXT")) {
        record("eglGetProcAddress", "name=%s success=%d", name, result != nullptr);
        if (result) {
            platform_display.store(reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(result));
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(traced_platform_display);
        }
    }
    return result;
}

struct NativeWindow;
struct NativeWindowBuffer;
struct Region {
    struct Rect { int32_t x, y; uint32_t w, h; } *rects;
    int32_t count;
};

extern "C" int NativeWindowRequestBuffer(NativeWindow *window, NativeWindowBuffer **buffer, int *fence)
{
    static auto original = next<decltype(&NativeWindowRequestBuffer)>("NativeWindowRequestBuffer");
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 4) record("queue.request", "event=begin call=%u", call);
    const int result = original ? original(window, buffer, fence) : -ENOSYS;
    if (call < 4) record("queue.request", "event=end call=%u code=%d buffer=%d", call, result,
                        result == 0 && buffer && *buffer);
    return result;
}

extern "C" int NativeWindowFlushBuffer(NativeWindow *window, NativeWindowBuffer *buffer, int fence, Region region)
{
    static auto original = next<decltype(&NativeWindowFlushBuffer)>("NativeWindowFlushBuffer");
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 4) record("queue.submit", "event=begin call=%u input=%d", call, buffer != nullptr);
    const int result = original ? original(window, buffer, fence, region) : -ENOSYS;
    if (call < 4) record("queue.submit", "event=end call=%u code=%d", call, result);
    return result;
}

namespace OHOS::Rosen {
namespace Drawing {
struct Canvas;
struct Data { bool BuildFromMalloc(const void *data, size_t length); };
struct Image { bool MakeFromEncoded(const std::shared_ptr<Data> &data); };

bool Data::BuildFromMalloc(const void *data, size_t length)
{
    static auto original = next<bool (*)(Data *, const void *, size_t)>(
        "_ZN4OHOS5Rosen7Drawing4Data15BuildFromMallocEPKvm");
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 4) record("frame.encoded-data", "event=begin call=%u bytes=%zu", call, length);
    const bool result = original && original(this, data, length);
    if (call < 4) record("frame.encoded-data", "event=end call=%u result=%d", call, result);
    return result;
}

bool Image::MakeFromEncoded(const std::shared_ptr<Data> &data)
{
    static auto original = next<bool (*)(Image *, const std::shared_ptr<Data> &)>(
        "_ZN4OHOS5Rosen7Drawing5Image15MakeFromEncodedERKNSt3__h10shared_ptrINS1_4DataEEE");
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 4) record("frame.decode", "event=begin call=%u input=%d", call, bool(data));
    const bool result = original && original(this, data);
    if (call < 4) record("frame.decode", "event=end call=%u result=%d", call, result);
    return result;
}
}

}

namespace {
constexpr uintptr_t request_begin = 0x1e8f4, request_end = 0x1e950;
constexpr uintptr_t draw_begin = 0x1ed24, draw_end = 0x1eda4;
constexpr uintptr_t submit_begin = 0x1e9c8, submit_end = 0x1ea0c;
std::atomic<unsigned> request_calls{0}, draw_calls{0}, submit_calls{0};
thread_local unsigned request_call = frame_limit, draw_call = frame_limit, submit_call = frame_limit;

uintptr_t player_offset(void *pc)
{
    const char *enabled = std::getenv("AURORA_BOOTANIMATION_TRACE_STOCK_CALLS");
    if (!enabled || std::strcmp(enabled, "1")) return 0;
    Dl_info symbol{};
    if (!dladdr(pc, &symbol) || !symbol.dli_fname ||
        std::strcmp(symbol.dli_fname, "/system/bin/bootanimation")) return 0;
    return reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(symbol.dli_fbase);
}

// The matched L410 player gets the active mode at 0x1de18, then stores
// (screenW,screenH) at +0x40 and a centered square (h,w,x,y) at +0x4c.
// OnDraw uses the latter fields at 0x1ed2c..0x1ed44. The shim receives x23
// before OnDraw reads them. No player-relative layout is used at other PCs.
void fit_panel_picture(void *picture_player)
{
    if (!picture_player || !panel_archive.load(std::memory_order_relaxed)) return;
    auto *object = static_cast<unsigned char *>(picture_player);
    int32_t screen[2];
    std::memcpy(screen, object + 0x40, sizeof(screen));
    const int32_t width = screen[0], height = screen[1];
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return;
    constexpr int32_t frame_width = 1512, frame_height = 982;
    int32_t image_width = width, image_height = height;
    if (int64_t(width) * frame_height > int64_t(height) * frame_width)
        image_width = (height * frame_width + frame_height / 2) / frame_height;
    else
        image_height = (width * frame_height + frame_width / 2) / frame_width;
    const int32_t rect[] = {image_height, image_width,
                            (width - image_width) / 2, (height - image_height) / 2};
    if (std::memcmp(object + 0x4c, rect, sizeof(rect)) != 0) {
        std::memcpy(object + 0x4c, rect, sizeof(rect));
        record("frame.layout", "screen=%dx%d x=%d y=%d width=%d height=%d",
               width, height, rect[2], rect[3], image_width, image_height);
    }
}

void begin_frame_stage(const char *stage, std::atomic<unsigned> &calls, unsigned &call)
{
    call = calls.fetch_add(1, std::memory_order_relaxed);
    if (trace_frame(call)) record(stage, "event=begin call=%u", call);
}
}

extern "C" __attribute__((visibility("hidden")))
void aurora_boot_trace_start(uint64_t tag, const void *name, float value, void *pc, void *picture_player)
{
    static auto original = next<void (*)(uint64_t, const void *, float)>("StartTrace");
    switch (player_offset(pc)) {
        case request_begin: begin_frame_stage("frame.request", request_calls, request_call); break;
        case draw_begin:
            fit_panel_picture(picture_player);
            begin_frame_stage("frame.draw", draw_calls, draw_call);
            break;
        case submit_begin: begin_frame_stage("frame.flush", submit_calls, submit_call); break;
    }
    if (original) original(tag, name, value);
}

extern "C" __attribute__((visibility("hidden")))
void aurora_boot_trace_finish(uint64_t tag, void *pc, const uintptr_t *caller_stack)
{
    static auto original = next<void (*)(uint64_t)>("FinishTrace");
    switch (player_offset(pc)) {
        case request_end:
            // In this player Draw passes its unique_ptr return slot as x8=sp
            // and reads that same word after the original FinishTrace call.
            if (trace_frame(request_call) || (request_call < frame_limit && !caller_stack[0]))
                record("frame.request", "event=end call=%u result=%d", request_call,
                       caller_stack[0] != 0);
            break;
        case draw_end:
            if (trace_frame(draw_call)) record("frame.draw", "event=end call=%u", draw_call);
            break;
        case submit_end:
            if (trace_frame(submit_call)) record("frame.flush", "event=end call=%u", submit_call);
            break;
    }
    if (original) original(tag);
}
