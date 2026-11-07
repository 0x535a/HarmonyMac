/* SPDX-License-Identifier: GPL-3.0-only
 * OHOS NativeWindow WSI over the OHOS build of Mesa's GBM softpipe backend.
 * A private GBM window supplies default-framebuffer semantics; each swap copies
 * pixels into the NativeWindow queue. No GL entry points are interposed.
 */
#include "include/aurora_drm.h"
#include "include/buffer_handle.h"
#include "include/display_type.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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
    AuroraDrmDisplay drm = {};
    gbm_device *device = nullptr;
    EGLDisplay egl = EGL_NO_DISPLAY;
};
std::mutex mutex;
Display display;
struct Window {
    EGLDisplay display;
    EGLConfig config;
    NativeWindow *native;
    EGLSurface backing = EGL_NO_SURFACE;
    gbm_surface *gbm = nullptr;
    int width = 0, height = 0;
    std::vector<EGLint> creation_attributes;
    bool destroyed = false;
    ~Window() {
        if (gbm) gbm_surface_destroy(gbm);
        if (native) native_api()->unreference(native);
    }
};
std::map<EGLSurface, std::shared_ptr<Window>> windows;
std::map<EGLDisplay, std::string> extension_strings;
thread_local std::shared_ptr<Window> current_draw, current_read;
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
bool create_backing(Window &window, int width, int height, const EGLint *attributes)
{
    auto get = real<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib");
    EGLint format;
    if (!get || !get(window.display, window.config, EGL_NATIVE_VISUAL_ID, &format)) return false;
    window.gbm = gbm_surface_create(display.device, width, height, format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!window.gbm) return false;
    auto create = real<decltype(&eglCreateWindowSurface)>("eglCreateWindowSurface");
    window.backing = create(window.display, window.config, reinterpret_cast<EGLNativeWindowType>(window.gbm), attributes);
    if (window.backing == EGL_NO_SURFACE) { gbm_surface_destroy(window.gbm); window.gbm = nullptr; return false; }
    window.width = width; window.height = height;
    return true;
}
bool resize(Window &window, int width, int height)
{
    if (window.width == width && window.height == height) return true;
    auto old_egl = window.backing; auto old_gbm = window.gbm;
    int old_width = window.width, old_height = window.height;
    window.gbm = nullptr;
    if (!create_backing(window, width, height, nullptr)) {
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
        return false;
    }
    real<decltype(&eglDestroySurface)>("eglDestroySurface")(window.display, old_egl);
    gbm_surface_destroy(old_gbm);
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
// Preserve application pack state, including ES3 PBO/row offsets and read FBO.
// Application GL errors are not consumed by the bridge.
bool copy_frame(Window &window, BufferHandle &buffer)
{
    std::vector<unsigned char> pixels;
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
    glReadPixels(0, 0, window.width, window.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (es3) {
        glReadBuffer(read_buffer); glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo); glPixelStorei(GL_PACK_ROW_LENGTH, row);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows); glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    } else glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    void *memory = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
    if (memory == MAP_FAILED) return false;
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync)) { munmap(memory, buffer.size); return false; }
    bool rgba = buffer.format == PIXEL_FMT_RGBA_8888 || buffer.format == PIXEL_FMT_RGBX_8888;
    for (int y = 0; y < buffer.height; ++y) {
        auto source = pixels.data() + size_t(window.height - 1 - uint64_t(y) * window.height / buffer.height) * window.width * 4;
        auto destination = static_cast<unsigned char *>(memory) + size_t(y) * buffer.stride;
        for (int x = 0; x < buffer.width; ++x) {
            auto pixel = source + uint64_t(x) * window.width / buffer.width * 4;
            destination[x*4] = pixel[rgba ? 0 : 2]; destination[x*4+1] = pixel[1];
            destination[x*4+2] = pixel[rgba ? 2 : 0]; destination[x*4+3] = pixel[3];
        }
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    int result = ioctl(buffer.fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(memory, buffer.size);
    return !result;
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
    if (!fn) { error(EGL_NOT_INITIALIZED); return EGL_NO_DISPLAY; }
    if (platform != PLATFORM_OHOS) return fn(platform, native, attributes);
    if (attributes && attributes[0] != EGL_NONE) { error(EGL_BAD_ATTRIBUTE); return EGL_NO_DISPLAY; }
    std::lock_guard<std::mutex> lock(mutex);
    if (display.egl != EGL_NO_DISPLAY) return display.egl;
    if (AuroraOpenDisplayDrm(&display.drm, 0)) { error(EGL_NOT_INITIALIZED); return EGL_NO_DISPLAY; }
    display.device = gbm_create_device(display.drm.fd);
    if (!display.device) { close(display.drm.fd); display.drm.fd = -1; error(EGL_NOT_INITIALIZED); return EGL_NO_DISPLAY; }
    display.egl = fn(EGL_PLATFORM_GBM_KHR, display.device, nullptr);
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
    if (window->backing!=EGL_NO_SURFACE && !fn(egl, window->backing)) return EGL_FALSE;
    window->destroyed = true; windows.erase(surface); return EGL_TRUE;
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
        if (attribute == EGL_BUFFER_AGE_EXT) { *value = 0; return EGL_TRUE; }
    }
    auto fn = real<decltype(&eglQuerySurface)>("eglQuerySurface"); return fn ? fn(egl, backing(surface), attribute, value) : error(EGL_NOT_INITIALIZED);
}
extern "C" EGLBoolean eglSwapBuffers(EGLDisplay egl, EGLSurface surface)
{
    std::lock_guard<std::mutex> lock(mutex); auto window = find(surface);
    auto swap = real<decltype(&eglSwapBuffers)>("eglSwapBuffers");
    if (!window) return swap ? swap(egl, surface) : error(EGL_NOT_INITIALIZED);
    if (egl != window->display || current_draw != window || current_read != window) return error(EGL_BAD_CURRENT_SURFACE);
    NativeBuffer *buffer = nullptr; int fence = -1;
    auto api = native_api();
    if (api->request(window->native, &buffer, &fence)) { if (fence >= 0) close(fence); return error(EGL_BAD_ALLOC); }
    auto handle = api->handle(buffer);
    bool waited = wait_fence(fence);
    if (!waited || !valid(handle) || !copy_frame(*window, *handle)) {
        api->abort(window->native, buffer); return error(EGL_BAD_ALLOC);
    }
    if (api->flush(window->native, buffer, -1, Region{nullptr, 0})) {
        api->abort(window->native, buffer); return error(EGL_BAD_NATIVE_WINDOW);
    }
    if (!swap(egl, window->backing)) return EGL_FALSE;
    int width, height;
    if (!queue_geometry(window->native, width, height) || !resize(*window, width, height)) return error(EGL_BAD_ALLOC);
    return EGL_TRUE;
}
extern "C" EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay egl, EGLSurface surface, const EGLint *, EGLint)
{ return eglSwapBuffers(egl, surface); }
extern "C" EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay egl, EGLSurface surface, const EGLint *, EGLint)
{ return eglSwapBuffers(egl, surface); }
extern "C" EGLBoolean eglSetDamageRegionKHR(EGLDisplay egl, EGLSurface surface, EGLint *, EGLint)
{ std::lock_guard<std::mutex> lock(mutex); auto window = find(surface); return window && window->display == egl ? EGL_TRUE : error(EGL_BAD_SURFACE); }

extern "C" EGLBoolean eglSurfaceAttrib(EGLDisplay egl, EGLSurface surface, EGLint attribute, EGLint value)
{ std::lock_guard<std::mutex> lock(mutex); auto fn = real<decltype(&eglSurfaceAttrib)>("eglSurfaceAttrib");
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
    if (target != NATIVE_BUFFER_OHOS) return fn(egl, context, target, object, attributes);
    if (context != EGL_NO_CONTEXT) { error(EGL_BAD_CONTEXT); return EGL_NO_IMAGE_KHR; }
    if (attributes) for (int n = 0; attributes[n] != EGL_NONE; n += 2) {
        if (attributes[n] != EGL_IMAGE_PRESERVED_KHR ||
            (attributes[n+1] != EGL_TRUE && attributes[n+1] != EGL_FALSE)) {
            error(EGL_BAD_ATTRIBUTE); return EGL_NO_IMAGE_KHR;
        }
    }
    auto api = native_api(); auto handle = api ? api->handle(reinterpret_cast<NativeBuffer *>(object)) : nullptr;
    if (!valid(handle)) { error(EGL_BAD_PARAMETER); return EGL_NO_IMAGE_KHR; }
    uint32_t format = handle->format == PIXEL_FMT_RGBA_8888 ? DRM_FORMAT_ABGR8888 :
        handle->format == PIXEL_FMT_RGBX_8888 ? DRM_FORMAT_XBGR8888 :
        handle->format == PIXEL_FMT_BGRA_8888 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    EGLint attrs[] = {EGL_WIDTH, handle->width, EGL_HEIGHT, handle->height, EGL_LINUX_DRM_FOURCC_EXT, int(format),
        EGL_DMA_BUF_PLANE0_FD_EXT, handle->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, handle->stride, EGL_NONE};
    return fn(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
}
extern "C" EGLBoolean eglReleaseThread()
{
    auto fn = real<decltype(&eglReleaseThread)>("eglReleaseThread"); if (!fn) return error(EGL_NOT_INITIALIZED);
    if (!fn()) return EGL_FALSE;
    current_draw.reset(); current_read.reset(); return EGL_TRUE;
}
extern "C" EGLImage eglCreateImage(EGLDisplay egl, EGLContext context, EGLenum target, EGLClientBuffer object, const EGLAttrib *attributes)
{
    if (target != NATIVE_BUFFER_OHOS) {
        auto fn = real<decltype(&eglCreateImage)>("eglCreateImage");
        return fn ? fn(egl, context, target, object, attributes) : EGL_NO_IMAGE;
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
#define FORWARD(ret, name, signature, arguments, fallback) \
extern "C" ret name signature { auto fn = real<decltype(&name)>(#name); return fn ? fn arguments : fallback; }
FORWARD(EGLBoolean, eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d,a,b), EGL_FALSE)
FORWARD(EGLBoolean, eglTerminate, (EGLDisplay d), (d), EGL_FALSE)
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
    INTERCEPT(eglSurfaceAttrib); INTERCEPT(eglBindTexImage); INTERCEPT(eglReleaseTexImage); INTERCEPT(eglCopyBuffers);
    INTERCEPT(eglSwapBuffersWithDamageKHR); INTERCEPT(eglSwapBuffersWithDamageEXT); INTERCEPT(eglSetDamageRegionKHR);
    INTERCEPT(eglCreateImageKHR); INTERCEPT(eglCreateImage); INTERCEPT(eglReleaseThread); INTERCEPT(eglGetError);
    INTERCEPT(eglQueryString);
#undef INTERCEPT
    auto fn = real<decltype(&eglGetProcAddress)>("eglGetProcAddress"); return fn ? fn(name) : nullptr;
}
