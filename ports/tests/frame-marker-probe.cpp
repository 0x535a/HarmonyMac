/* Frame-content diagnostics oracle: non-scanout EGL window (GPU blit path),
 * serial marker, source and scanout fingerprints, and cached blit imports. */
#include "buffer_handle.h"
#include "composer_abi.h"
#include "display_type.h"
#include "frame_samples.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using AuroraComposer::Hotplug;
using AuroraComposer::Mode;
namespace
{
constexpr int frame_count = 24;
int width, height;
extern "C" __attribute__((visibility("default"))) int GetParameter(const char *key, const char *, char *value,
                                                                   unsigned size)
{
    const char *result = !std::strcmp(key, "bootevent.boot.completed") ? "true" : "false";
    if (std::strlen(result) >= size)
        return -1;
    std::strcpy(value, result);
    return static_cast<int>(std::strlen(result));
}
struct Listener {
    void *context;
    void (*callback)(void *);
};
void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "MARKER_FAIL %s errno=%d egl=%#x gl=%#x\n", what, errno, eglGetError(), glGetError());
        std::exit(2);
    }
}
template <class T> T symbol(void *library, const char *name)
{
    T p = reinterpret_cast<T>(dlsym(library, name));
    check(p != nullptr, name);
    return p;
}
void *load(const char *path)
{
    void *p = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!p)
        fprintf(stderr, "%s: %s\n", path, dlerror());
    check(p != nullptr, path);
    return p;
}
void connected(uint32_t device, bool yes, void *cookie)
{
    if (!device && yes)
        *static_cast<bool *>(cookie) = true;
}
void notify(void *) {}
using RGB = std::array<unsigned char, 3>;
RGB background(int frame) { return {static_cast<unsigned char>(20 + 7 * frame), 100, static_cast<unsigned char>(200 - 5 * frame)}; }
RGB box_color(int frame) { return {250, static_cast<unsigned char>(10 * frame), 30}; }
// Top-left origin. Kept away from the marker rows so its damage is explicit.
void box(int frame, int &x, int &y, int &w, int &h) { x = 9 + 11 * frame; y = 40 + 5 * frame; w = 31; h = 17; }
RGB expected(int frame, int x, int y)
{
    int bx, by, bw, bh;
    box(frame, bx, by, bw, bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh)
        return box_color(frame);
    return background(frame);
}
void clear_rect(int x, int y, int w, int h, RGB c)
{
    glScissor(x, height - y - h, w, h);
    glClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
// Returns the decoded serial; checks every non-marker pixel exactly.
int verify(const BufferHandle *h, int frame, unsigned &mismatches)
{
    using namespace AuroraFrameSamples;
    check(h && h->width == width && h->height == height && h->format == PIXEL_FMT_BGRA_8888, "consumer geometry");
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU acquire");
    void *map = mmap(nullptr, h->size, PROT_READ, MAP_SHARED, h->fd, 0);
    check(map != MAP_FAILED, "frame mapping");
    auto p = static_cast<const unsigned char *>(map);
    const int left = marker_left(width), right = left + marker_cells * marker_cell;
    mismatches = 0;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            if (y < marker_rows && x >= left && x < right)
                continue;
            RGB target = expected(frame, x, y);
            const unsigned char *px = p + y * h->stride + x * 4;
            if (px[2] != target[0] || px[1] != target[1] || px[0] != target[2]) {
                if (!mismatches)
                    fprintf(stderr, "MARKER_MISMATCH frame=%d xy=%d,%d expected=%u,%u,%u found=%u,%u,%u\n", frame,
                            x, y, target[0], target[1], target[2], px[2], px[1], px[0]);
                mismatches++;
            }
        }
    const Result result = sample(p, width, height, h->stride, h->format);
    check(munmap(map, h->size) == 0, "unmap");
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU release");
    return result.serial;
}
} // namespace
int main()
{
    setbuf(stdout, nullptr);
    alarm(60);
    void *composer = load("/system/bin/aurora/marker-test/libdisplay_composer_vdi_impl.z.so");
    auto plug = symbol<int32_t (*)(Hotplug, void *)>(composer, "RegHotPlugCallback");
    auto modes = symbol<int32_t (*)(uint32_t, std::vector<Mode> *)>(composer, "GetDisplaySupportedModes");
    auto client = symbol<int32_t (*)(uint32_t, const BufferHandle *, int32_t)>(composer, "SetDisplayClientBuffer");
    auto commit = symbol<int32_t (*)(uint32_t, int32_t *)>(composer, "Commit");
    auto clear = symbol<int32_t (*)(uint32_t)>(composer, "ClearDisplayClientBuffer");
    std::vector<Mode> available;
    bool online = false;
    check(!plug(connected, &online) && online && !modes(0, &available) && !available.empty(), "virtual primary");
    width = available[0].width;
    height = available[0].height;
    check(AuroraFrameSamples::marker_fits(width, height), "marker geometry");
    void *surface = load("/system/lib64/chipset-pub-sdk/libsurface.z.so");
    void *image = load("/system/lib64/libnative_image.so");
    auto create = symbol<void *(*)()>(image, "OH_ConsumerSurface_Create");
    auto set_size = symbol<int (*)(void *, int, int)>(image, "OH_ConsumerSurface_SetDefaultSize");
    auto usage = symbol<int (*)(void *, uint64_t)>(image, "OH_ConsumerSurface_SetDefaultUsage");
    auto listen = symbol<int (*)(void *, Listener)>(image, "OH_NativeImage_SetOnFrameAvailableListener");
    auto getwindow = symbol<void *(*)(void *)>(image, "OH_NativeImage_AcquireNativeWindow");
    auto acquire = symbol<int (*)(void *, void **, int *)>(image, "OH_NativeImage_AcquireNativeWindowBuffer");
    auto release = symbol<int (*)(void *, void *, int)>(image, "OH_NativeImage_ReleaseNativeWindowBuffer");
    auto destroy = symbol<void (*)(void **)>(image, "OH_NativeImage_Destroy");
    auto opt = symbol<int (*)(void *, int, ...)>(surface, "OH_NativeWindow_NativeWindowHandleOpt");
    auto unref = symbol<int (*)(void *)>(surface, "OH_NativeWindow_NativeObjectUnreference");
    auto handle = symbol<BufferHandle *(*)(void *)>(surface, "OH_NativeWindow_GetBufferHandleFromNative");
    // A RenderService composition target: GPU-rendered and texture-sampled, not a framebuffer.
    const uint64_t flags = HBM_USE_MEM_DMA | HBM_USE_CPU_READ | HBM_USE_CPU_WRITE | HBM_USE_HW_RENDER |
                           HBM_USE_HW_TEXTURE;
    void *consumer = create();
    check(consumer && !set_size(consumer, width, height) && !usage(consumer, flags) &&
              !listen(consumer, {nullptr, notify}),
          "consumer setup");
    void *window = getwindow(consumer);
    check(window && !opt(window, 0, width, height) && !opt(window, 3, PIXEL_FMT_BGRA_8888) && !opt(window, 5, flags),
          "producer setup");
    EGLDisplay d = eglGetPlatformDisplayEXT(0x34e0, nullptr, nullptr);
    check(d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_ES_API), "initialize");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfgs[128], cfg = nullptr;
    EGLint n = 0;
    check(eglChooseConfig(d, attrs, cfgs, 128, &n), "configs");
    for (int i = 0; i < n; i++) {
        EGLint visual = 0;
        if (eglGetConfigAttrib(d, cfgs[i], EGL_NATIVE_VISUAL_ID, &visual) && visual == 0x34325241) {
            cfg = cfgs[i];
            break;
        }
    }
    check(cfg, "AR24 visual");
    EGLSurface win = eglCreateWindowSurface(d, cfg, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    const EGLint ctxattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxattrs);
    check(win != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(d, win, win, context), "bind");
    glViewport(0, 0, width, height);
    // Application state the bridge must preserve across its marker clears.
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    std::set<ino_t> buffers;
    void *previous = nullptr;
    unsigned decoded = 0;
    for (int frame = 0; frame < frame_count; frame++) {
        EGLint age = 0;
        check(eglQuerySurface(d, win, EGL_BUFFER_AGE_EXT, &age), "age");
        clear_rect(0, 0, width, height, background(frame));
        int bx, by, bw, bh;
        box(frame, bx, by, bw, bh);
        clear_rect(bx, by, bw, bh, box_color(frame));
        GLint scissor[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        const EGLint damage[] = {0, 0, width, height};
        check(eglSwapBuffersWithDamageKHR(d, win, damage, 1), "swap");
        GLint after[4];
        glGetIntegerv(GL_SCISSOR_BOX, after);
        check(glIsEnabled(GL_SCISSOR_TEST) && !std::memcmp(scissor, after, sizeof(after)), "GL state preserved");
        void *received = nullptr;
        int fence = -1;
        check(!acquire(consumer, &received, &fence) && received, "acquire");
        if (fence >= 0) {
            pollfd ready = {fence, POLLIN, 0};
            check(poll(&ready, 1, 2000) == 1, "producer fence");
        }
        const BufferHandle *h = handle(received);
        unsigned mismatches = 0;
        const int serial = verify(h, frame, mismatches);
        struct stat info {};
        check(!fstat(h->fd, &info), "identity");
        buffers.insert(info.st_ino);
        printf("MARKER_FRAME index=%d serial=%d mismatches=%u inode=%llu age=%d\n", frame, serial, mismatches,
               (unsigned long long)info.st_ino, age);
        check(!mismatches, "content outside the marker is unchanged");
        check(serial == frame + 1, "consumer decodes the producer serial");
        ++decoded;
        check(!client(0, h, fence), "set client");
        if (fence >= 0)
            close(fence);
        int32_t out_fence = -1;
        check(!commit(0, &out_fence), "commit");
        if (frame == 12) {
            // Positive control: a write into the buffer KMS is scanning must be
            // reported when that buffer is replaced.
            dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
            check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU write acquire");
            void *map = mmap(nullptr, h->size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
            check(map != MAP_FAILED, "writable mapping");
            for (int y = height / 2; y < height / 2 + 40; ++y)
                std::memset(static_cast<unsigned char *>(map) + y * h->stride + width / 2 * 4, 0x5a, 60 * 4);
            check(munmap(map, h->size) == 0, "unmap written");
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
            check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU write release");
            printf("MARKER_SCANNED_WRITE index=%d\n", frame);
        }
        if (previous)
            check(!release(consumer, previous, -1), "release previous");
        previous = received;
    }
    check(buffers.size() >= 2 && buffers.size() <= 4, "native buffers rotate");
    check(!clear(0), "clear retains the displayed frame");
    check(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) && eglDestroySurface(d, win) &&
              eglDestroyContext(d, context) && eglTerminate(d),
          "teardown with cached blit images");
    check(!release(consumer, previous, -1), "release last");
    check(!unref(window), "release producer");
    destroy(&consumer);
    printf("MARKER_PROBE_PASS frames=%d decoded=%u buffers=%zu\n", frame_count, decoded, buffers.size());
}
