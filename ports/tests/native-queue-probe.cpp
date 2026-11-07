/* Public C API exercise of the ISO's unchanged ConsumerSurface/NativeWindow. */
#include "buffer_handle.h"
#include "display_type.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#ifdef NATIVE_QUEUE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#endif
template<typename T> static T symbol(void *lib, const char *name)
{
    auto value = reinterpret_cast<T>(dlsym(lib, name));
    if (!value) { fprintf(stderr, "NATIVE_QUEUE_MISSING %s\n", name); exit(2); }
    return value;
}
static void check(bool value, const char *what)
{ if (!value) { fprintf(stderr, "NATIVE_QUEUE_FAIL %s\n", what); exit(2); } }
static std::atomic<int> frames{0};
static void frame(void *) { ++frames; }
struct Listener { void *context; void (*callback)(void *); };
struct Region { struct Rect { int32_t x, y; uint32_t w, h; } *rects; int32_t count; };
static void wait_fence(int fd)
{
    if (fd < 0) return;
    pollfd p{fd, POLLIN, 0};
    check(poll(&p, 1, 3000) == 1 && !(p.revents & (POLLERR | POLLNVAL)), "buffer fence");
    close(fd);
}
static void sync_buffer(int fd, uint64_t flags)
{
    dma_buf_sync s{flags};
    check(!ioctl(fd, DMA_BUF_IOCTL_SYNC, &s), "DMA-BUF synchronization");
}
int main()
{
    setbuf(stdout, nullptr); alarm(20);
    void *image_lib = dlopen("/system/lib64/libnative_image.so", RTLD_NOW | RTLD_LOCAL);
    if (!image_lib) { puts(dlerror()); return 2; }
    auto create = symbol<void *(*)()>(image_lib, "OH_ConsumerSurface_Create");
    auto size = symbol<int (*)(void *, int, int)>(image_lib, "OH_ConsumerSurface_SetDefaultSize");
    auto usage = symbol<int (*)(void *, uint64_t)>(image_lib, "OH_ConsumerSurface_SetDefaultUsage");
    auto native_window = symbol<void *(*)(void *)>(image_lib, "OH_NativeImage_AcquireNativeWindow");
    auto listener = symbol<int (*)(void *, Listener)>(image_lib, "OH_NativeImage_SetOnFrameAvailableListener");
    auto destroy = symbol<void (*)(void **)>(image_lib, "OH_NativeImage_Destroy");
    auto acquire = symbol<int (*)(void *, void **, int *)>(image_lib, "OH_NativeImage_AcquireNativeWindowBuffer");
    auto release = symbol<int (*)(void *, void *, int)>(image_lib, "OH_NativeImage_ReleaseNativeWindowBuffer");
    void *surface_lib = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
    check(surface_lib, "original surface library");
    auto request = symbol<int (*)(void *, void **, int *)>(surface_lib, "OH_NativeWindow_NativeWindowRequestBuffer");
    auto abort = symbol<int (*)(void *, void *)>(surface_lib, "OH_NativeWindow_NativeWindowAbortBuffer");
    auto unref = symbol<int (*)(void *)>(surface_lib, "OH_NativeWindow_NativeObjectUnreference");
    auto option = symbol<int (*)(void *, int, ...)>(surface_lib, "OH_NativeWindow_NativeWindowHandleOpt");
#ifndef NATIVE_QUEUE_EGL
    auto flush = symbol<int (*)(void *, void *, int, Region)>(surface_lib, "OH_NativeWindow_NativeWindowFlushBuffer");
#endif
    auto handle = symbol<BufferHandle *(*)(void *)>(surface_lib, "OH_NativeWindow_GetBufferHandleFromNative");
    void *image = create(); check(image, "consumer creation");
    check(!size(image, 13, 7) && !usage(image, HBM_USE_CPU_READ | HBM_USE_CPU_WRITE), "consumer size/usage");
    check(!listener(image, Listener{nullptr, frame}), "consumer listener");
    void *window = native_window(image); check(window, "producer native window");
    check(!option(window, 0, 13, 7) && !option(window, 3, PIXEL_FMT_RGBA_8888), "producer geometry/format");
    puts("ORIGINAL_NATIVE_QUEUE_CREATED");
    void *buffer = nullptr; int fence = -1;
    int result = request(window, &buffer, &fence);
    printf("ORIGINAL_NATIVE_QUEUE_REQUEST result=%d buffer=%p fence=%d\n", result, buffer, fence);
    wait_fence(fence);
    if (!result && buffer) check(!abort(window, buffer), "abort requested buffer");
    if (result || !buffer) return 2;
    puts("ORIGINAL_NATIVE_QUEUE_REQUEST_PASS");
#ifdef NATIVE_QUEUE_EGL
    EGLDisplay display = eglGetPlatformDisplayEXT(0x34e0, nullptr, nullptr);
    check(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "real queue EGL display");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count;
    check(eglChooseConfig(display, attrs, &config, 1, &count) && count, "real queue EGL config");
    const EGLint ctxattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxattrs);
    EGLSurface eglsurface = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    check(context != EGL_NO_CONTEXT && eglsurface != EGL_NO_SURFACE &&
          eglMakeCurrent(display, eglsurface, eglsurface, context), "real queue EGL current");
    printf("ORIGINAL_NATIVE_QUEUE_EGL_RENDERER %s\n", glGetString(GL_RENDERER));
#endif
    for (int n = 0; n < 6; ++n) {
        int width = 13, height = 7;
#ifdef NATIVE_QUEUE_EGL
        if (n >= 3) { width = 19; height = 9; }
        check(!size(image, width, height) && !option(window, 0, width, height), "real queue resize");
        // EGL resizes its backing after the first swap at the new window size.
        int render_width = n == 3 ? 13 : width, render_height = n == 3 ? 7 : height;
        glViewport(0, 0, render_width, render_height);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(n / 255.0f, 1, 64.0f / 255, 1); glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor(0, 0, render_width, render_height / 2);
        glClearColor(1, n / 255.0f, 64.0f / 255, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        check(glGetError() == GL_NO_ERROR && eglSwapBuffers(display, eglsurface), "real queue EGL swap");
#else
        buffer = nullptr; fence = -1;
        check(!request(window, &buffer, &fence) && buffer, "request reusable buffer");
        wait_fence(fence);
        BufferHandle *h = handle(buffer);
        check(h && h->virAddr && h->width == 13 && h->height == 7 && h->stride >= 52,
              "original mapper address/geometry");
        sync_buffer(h->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        auto pixels = static_cast<unsigned char *>(h->virAddr);
        for (int y = 0; y < h->height; ++y) for (int x = 0; x < h->width * 4; ++x)
            pixels[y * h->stride + x] = static_cast<unsigned char>(n * 17 + y * 3 + x);
        sync_buffer(h->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        check(!flush(window, buffer, -1, Region{nullptr, 0}), "flush original queue");
#endif
        void *received = nullptr; fence = -1;
        check(!acquire(image, &received, &fence) && received, "consume original queue");
        wait_fence(fence);
        BufferHandle *r = handle(received);
        check(r && r->width == width && r->height == height && r->size >= r->stride * height, "consumer buffer layout");
        void *mapped = mmap(nullptr, r->size, PROT_READ, MAP_SHARED, r->fd, 0);
        check(mapped != MAP_FAILED, "consumer DMA-BUF mapping");
        sync_buffer(r->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        auto actual = static_cast<unsigned char *>(mapped);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width * 4; ++x)
#ifdef NATIVE_QUEUE_EGL
        {
            bool red = uint64_t(render_height - 1) - uint64_t(y) * render_height / height < uint64_t(render_height / 2);
            int channel = x % 4;
            if (r->format == PIXEL_FMT_BGRA_8888 && (channel == 0 || channel == 2)) channel = 2 - channel;
            unsigned char expected = channel == 3 ? 255 : channel == 2 ? 64 :
                channel == 0 ? (red ? 255 : n) : (red ? n : 255);
            if (actual[y * r->stride + x] != expected) {
                fprintf(stderr, "QUEUE_PIXEL_MISMATCH frame=%d xbyte=%d y=%d actual=%u expected=%u format=%d render=%dx%d buffer=%dx%d\n",
                        n, x, y, actual[y * r->stride + x], expected, r->format,
                        render_width, render_height, width, height);
                check(false, "original EGL queue pixels/orientation");
            }
        }
#else
            check(actual[y * r->stride + x] == static_cast<unsigned char>(n * 17 + y * 3 + x),
                  "original queue pixel preservation");
#endif
        sync_buffer(r->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        check(!munmap(mapped, r->size), "consumer unmap");
        check(!release(image, received, -1), "release consumer buffer");
        printf("ORIGINAL_NATIVE_QUEUE_FRAME_PASS frame=%d\n", n);
    }
    check(frames.load() == 6, "original frame listener callbacks");
#ifdef NATIVE_QUEUE_EGL
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) &&
          eglDestroySurface(display, eglsurface) && eglDestroyContext(display, context) &&
          eglTerminate(display), "real queue EGL teardown");
    puts("ORIGINAL_NATIVE_QUEUE_EGL_PASS");
#endif
    unref(window); destroy(&image);
    puts("ORIGINAL_NATIVE_QUEUE_FULL_PASS");
    return 0;
}
