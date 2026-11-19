/* Pixel oracle for the OHOS EGL bridge, NativeImage queue and Composer scanout. */
#include "buffer_handle.h"
#include "composer_abi.h"
#include "display_type.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <map>
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
constexpr int frame_count = 36;
int width, height;
// Run a steady desktop scene after boot. Make the local virtual KMS exercise
// the requested path immediately instead of waiting for real desktop startup.
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
using RGB = std::array<unsigned char, 3>;
struct Box {
    int x, y, width, height;
};
void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "CYCLE_FAIL %s errno=%d egl=%#x gl=%#x\n", what, errno, eglGetError(), glGetError());
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
Box cursor(int frame) { return {(11 + 17 * frame) % (width - 23), (7 + 13 * frame) % (height - 19), 23, 19}; }
bool contains(Box b, int x, int y) { return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height; }
RGB base_color(int frame, int x, int y)
{
    // A horizontal division away from the midline exposes vertical inversions.
    const int scene = frame / 12;
    return {static_cast<unsigned char>(30 + 57 * scene), static_cast<unsigned char>(y < height / 3 ? 96 : 26),
            static_cast<unsigned char>(x < width / 4 ? 140 : 53)};
}
RGB cursor_color(int frame)
{
    return {static_cast<unsigned char>(240 - frame), 200, static_cast<unsigned char>(6 * frame)};
}
void fill(Box b, RGB rgb)
{
    glScissor(b.x, height - b.y - b.height, b.width, b.height);
    glClearColor(rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
void background(int frame, Box damage)
{
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 2; col++) {
            int x = std::max(damage.x, col ? width / 4 : 0);
            int y = std::max(damage.y, row ? height / 3 : 0);
            int r = std::min(damage.x + damage.width, col ? width : width / 4);
            int b = std::min(damage.y + damage.height, row ? height : height / 3);
            if (x < r && y < b)
                fill({x, y, r - x, b - y}, base_color(frame, x, y));
        }
}
constexpr int tiles = 8;
Box tile(int index) { return {width / 8 + (index % 4) * 37, height / 8 + (index / 4) * 29, 11, 13}; }
RGB tile_color(int frame, int index)
{
    const int last = frame - (frame + tiles - index) % tiles;
    return {static_cast<unsigned char>(73 + 3 * last), static_cast<unsigned char>(13 + 23 * index), 211};
}
RGB expected_pixel(int frame, int x, int y)
{
    if (contains(cursor(frame), x, y))
        return cursor_color(frame);
    for (int i = 0; i < tiles; i++)
        if (frame >= i && contains(tile(i), x, y))
            return tile_color(frame, i);
    return base_color(frame, x, y);
}
Box intersection(Box a, Box b)
{
    int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    int right = std::min(a.x + a.width, b.x + b.width), bottom = std::min(a.y + a.height, b.y + b.height);
    return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}
void paint_region(int frame, Box damage)
{
    background(frame, damage);
    for (int i = 0; i < tiles; i++) {
        Box changed = intersection(tile(i), damage);
        if (frame >= i && changed.width && changed.height)
            fill(changed, tile_color(frame, i));
    }
    Box box = intersection(cursor(frame), damage);
    if (box.width && box.height)
        fill(box, cursor_color(frame));
}
std::vector<Box> repair_region(int frame, int age, int first_frame)
{
    check(age >= 0, "valid EGL buffer age");
    if (!age || age > frame - first_frame || (frame - age) / 12 != frame / 12)
        return {{0, 0, width, height}};
    std::vector<Box> result;
    for (int step = frame - age + 1; step <= frame; step++) {
        result.push_back(cursor(step - 1));
        result.push_back(cursor(step));
        result.push_back(tile(step % tiles));
    }
    return result;
}
std::vector<EGLint> egl_region(const std::vector<Box> &damage)
{
    std::vector<EGLint> rects;
    for (Box box : damage)
        rects.insert(rects.end(), {box.x, height - box.y - box.height, box.width, box.height});
    return rects;
}
void draw(int frame, const std::vector<Box> &damage)
{
    glEnable(GL_SCISSOR_TEST);
    for (Box box : damage)
        paint_region(frame, box);
    glDisable(GL_SCISSOR_TEST);
    check(glGetError() == GL_NO_ERROR, "history damage redraw");
}
void validate_history_oracle()
{
    // Some EGL surfaces correctly report zero for every frame. Test the
    // positive-age damage algorithm independently on an actual 3-FBO ring.
    std::array<GLuint, 3> fbos{}, storage{};
    glGenFramebuffers(fbos.size(), fbos.data());
    glGenRenderbuffers(storage.size(), storage.data());
    for (size_t i = 0; i < fbos.size(); ++i) {
        glBindRenderbuffer(GL_RENDERBUFFER, storage[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, storage[i]);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "history oracle FBO");
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    auto mismatches = [&](int frame) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        check(glGetError() == GL_NO_ERROR, "history oracle read");
        unsigned errors = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto expected = expected_pixel(frame, x, height - 1 - y);
                if (std::memcmp(pixels.data() + (static_cast<size_t>(y) * width + x) * 4, expected.data(), 3) != 0)
                    ++errors;
            }
        return errors;
    };
    unsigned negative_errors = 0;
    for (int frame = 0; frame < 12; ++frame) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[frame % 3]);
        if (frame == 4) {
            draw(frame, repair_region(frame, 1, 0));
            negative_errors = mismatches(frame);
            check(negative_errors > 0, "single-frame-only damage must expose stale pixels on the third buffer");
        }
        draw(frame, repair_region(frame, frame < 3 ? 0 : 3, 0));
        check(!mismatches(frame), "all historical damage repairs the three-buffer ring");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(fbos.size(), fbos.data());
    glDeleteRenderbuffers(storage.size(), storage.data());
    printf("CYCLE_AGE_ORACLE frames=12 positive_age=3 negative_mismatches=%u\n", negative_errors);
}
void verify(const BufferHandle *h, int frame)
{
    check(h && h->width == width && h->height == height && h->stride >= width * 4 &&
              (h->format == PIXEL_FMT_BGRX_8888 || h->format == PIXEL_FMT_BGRA_8888),
          "scanout geometry");
    dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU acquire");
    void *map = mmap(nullptr, h->size, PROT_READ, MAP_SHARED, h->fd, 0);
    check(map != MAP_FAILED, "frame mapping");
    auto p = static_cast<const unsigned char *>(map);
    unsigned mismatch = 0;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            RGB target = expected_pixel(frame, x, y);
            const unsigned char *px = p + y * h->stride + x * 4;
            if (px[0] != target[2] || px[1] != target[1] || px[2] != target[0]) {
                if (!mismatch)
                    fprintf(stderr, "CYCLE_MISMATCH frame=%d xy=%d,%d expected=%u,%u,%u found=%u,%u,%u\n", frame,
                            x, y, target[0], target[1], target[2], px[2], px[1], px[0]);
                mismatch++;
            }
        }
    check(munmap(map, h->size) == 0, "unmap");
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    check(ioctl(h->fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU release");
    check(!mismatch, "consumer pixel oracle");
}
} // namespace
int main()
{
    setbuf(stdout, nullptr);
    alarm(65);
    void *composer = load("/system/bin/aurora/cycle-test/libdisplay_composer_vdi_impl.z.so");
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
    check(width > 64 && height > 64, "usable geometry");
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
    void *consumer = nullptr, *window = nullptr;
    auto new_queue = [&] {
        consumer = create();
        const uint64_t flags = HBM_USE_MEM_FB | HBM_USE_CPU_READ | HBM_USE_HW_RENDER;
        check(consumer && !set_size(consumer, width, height) && !usage(consumer, flags) &&
                  !listen(consumer, {nullptr, notify}),
              "stock consumer setup");
        window = getwindow(consumer);
        check(window && !opt(window, 0, width, height) && !opt(window, 3, PIXEL_FMT_BGRA_8888) &&
                  !opt(window, 5, flags),
              "stock producer setup");
    };
    new_queue();
    EGLDisplay d = eglGetPlatformDisplayEXT(0x34e0, nullptr, nullptr);
    check(d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_ES_API),
          "initialize OHOS platform");
    const EGLint attrs[] = {EGL_SURFACE_TYPE,
                            EGL_WINDOW_BIT,
                            EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT,
                            EGL_RED_SIZE,
                            8,
                            EGL_GREEN_SIZE,
                            8,
                            EGL_BLUE_SIZE,
                            8,
                            EGL_ALPHA_SIZE,
                            8,
                            EGL_NONE};
    EGLConfig cfgs[128], cfg = nullptr;
    EGLint n = 0;
    check(eglChooseConfig(d, attrs, cfgs, 128, &n), "enumerate configs");
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
    check(win != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(d, win, win, context),
          "bind OHOS surface");
    const int non_dmabuf = open("/dev/null", O_RDONLY | O_CLOEXEC);
    check(non_dmabuf >= 0, "open non-DMA-BUF fixture");
    const EGLint bad_image[] = {
        EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_LINUX_DRM_FOURCC_EXT, 0x34325258,
        EGL_DMA_BUF_PLANE0_FD_EXT, non_dmabuf, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, 64, EGL_NONE,
    };
    check(eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, bad_image) == EGL_NO_IMAGE_KHR,
          "reject a non-DMA-BUF image");
    const EGLint image_error = eglGetError();
    check(image_error != EGL_SUCCESS && eglGetError() == EGL_SUCCESS, "preserve the caller's import error");
    close(non_dmabuf);
    printf("CYCLE_IMAGE_IMPORT_ERROR_PRESERVED code=%#x\n", image_error);
    EGLint swap_behavior = 0;
    check(eglQuerySurface(d, win, EGL_SWAP_BEHAVIOR, &swap_behavior), "query public swap behavior");
    const char *extensions = eglQueryString(d, EGL_EXTENSIONS);
    printf("CYCLE_CAPS vendor=%s version=%s renderer=%s partial_update=%d buffer_age=%d swap_behavior=%#x\n",
           eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION), glGetString(GL_RENDERER),
           extensions && std::strstr(extensions, "EGL_KHR_partial_update") != nullptr,
           extensions && std::strstr(extensions, "EGL_EXT_buffer_age") != nullptr, swap_behavior);
    glViewport(0, 0, width, height);
    validate_history_oracle();
    std::set<ino_t> buffers;
    void *previous = nullptr, *previous_consumer = nullptr, *previous_window = nullptr;
    std::map<ino_t, int> last_draw;
    int first_frame = 0;
    unsigned prior_visible = 0, prior_acquired = 0;
    for (int frame = 0; frame < frame_count; frame++) {
        if (frame == frame_count / 2) {
            check(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) && eglDestroySurface(d, win),
                  "recreate scanout pool");
            // The previous consumer and its acquired presentation remain alive across
            // destruction of the old EGL surface and creation of the new pool.
            new_queue();
            win = eglCreateWindowSurface(d, cfg, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
            check(win != EGL_NO_SURFACE && eglMakeCurrent(d, win, win, context), "bind new producer pool");
            first_frame = frame;
            last_draw.clear();
        }
        EGLint age = 0;
        check(eglQuerySurface(d, win, EGL_BUFFER_AGE_EXT, &age), "query age");
        if (frame == first_frame)
            check(age == 0, "new surface has no previous frame");
        auto damage = repair_region(frame, age, first_frame);
        bool full = damage.size() == 1;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        auto declared = egl_region(damage);
        EGLBoolean declared_ok = eglSetDamageRegionKHR(d, win, declared.data(), declared.size() / 4);
        if (!declared_ok) {
            EGLint error = eglGetError();
            printf("CYCLE_DAMAGE_FAIL index=%d age=%d rects=%zu returned=%u egl_error=%#x\n", frame, age,
                   declared.size() / 4, declared_ok, error);
            check(false, "declare historical buffer repair");
        }
        draw(frame, damage);
        // The repair above covers intervening history. Presentation damage
        // refers only to changes since the previous displayed frame.
        auto present_damage = egl_region(repair_region(frame, 1, first_frame));
        check(eglSwapBuffersWithDamageKHR(d, win, present_damage.data(), present_damage.size() / 4),
              "publish frame with display damage");
        void *received = nullptr;
        int fence = -1;
        const int acquired = acquire(consumer, &received, &fence);
        if (acquired || !received)
            printf("CYCLE_ACQUIRE_FAIL frame=%d result=%d ptr=%p fence=%d\n", frame, acquired, received, fence);
        check(!acquired && received, "stock acquire");
        const BufferHandle *h = handle(received);
        if (fence >= 0) {
            pollfd ready = {fence, POLLIN, 0};
            check(poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN), "producer fence");
        }
        verify(h, frame);
        if (previous) {
            verify(handle(previous), frame - 1);
            prior_acquired++;
            printf("CYCLE_PENDING index=%d visible=%d size=%dx%d\n", frame, frame - 1, width, height);
            check(getchar() == '\n', "scanout stays on previous buffer before replacement");
            prior_visible++;
        }
        // Production has separate processes. This probe calls both libraries
        // in one process, so use independent trace directories for their PID.
        if (!frame)
            setenv("AURORA_PERF_TRACE_DIR", "/data/local/tmp/aurora-cycle-composer", 1);
        check(!client(0, h, fence), "set exact client");
        if (fence >= 0)
            close(fence);
        int32_t out_fence = -1;
        check(!commit(0, &out_fence), "KMS commit");
        if (out_fence >= 0) {
            // Asynchronous commit: the present fence signals once the frame is on screen.
            pollfd presented = {out_fence, POLLIN, 0};
            check(poll(&presented, 1, 2000) == 1 && (presented.revents & POLLIN), "present fence");
            close(out_fence);
        }
        struct stat info{};
        check(!fstat(h->fd, &info), "BO identity");
        buffers.insert(info.st_ino);
        if (age) {
            auto old = last_draw.find(info.st_ino);
            check(old != last_draw.end() && frame - old->second == age,
                  "nonzero age names last draw of this actual BO");
        }
        last_draw[info.st_ino] = frame;
        printf("CYCLE_FRAME index=%d age=%d inode=%llu size=%dx%d pool_start=%d full=%d\n", frame, age,
               (unsigned long long)info.st_ino, width, height, first_frame, full ? 1 : 0);
        // Keep the old native buffer acquired through the actual replacement flip.
        if (previous) {
            verify(handle(previous), frame - 1);
            check(!release(previous_consumer, previous, -1), "retire prior frame after KMS completion");
            if (previous_consumer != consumer) {
                check(!unref(previous_window), "release retired native producer");
                destroy(&previous_consumer);
            }
        }
        previous = received;
        previous_consumer = consumer;
        previous_window = window;
        check(getchar() == '\n', "QMP scanout checkpoint");
    }
    printf("CYCLE_LIFETIME checked_acquired=%u checked_scanout=%u\n", prior_acquired, prior_visible);
    check(buffers.size() >= 2 && buffers.size() < frame_count, "BOs rotate and are reused");
    check(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "unbind prior to EGL teardown");
    // Legal eglTerminate with a live, unbound EGLSurface and a displayed
    // buffer still acquired by the native consumer. The old EGL handles
    // are invalidated implicitly and must not be destroyed again.
    check(eglGetCurrentContext() == EGL_NO_CONTEXT && eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE,
          "terminated surface was unbound");
    check(eglTerminate(d), "terminate with undestroyed noncurrent EGL surface");
    verify(handle(previous), frame_count - 1);
    printf("CYCLE_TERMINATED visible=%d size=%dx%d\n", frame_count - 1, width, height);
    check(getchar() == '\n', "final scanout stays intact after eglTerminate");
    check(!clear(0) && !release(consumer, previous, -1), "retire last native frame after EGL teardown");
    check(!unref(window), "drop terminated native producer");
    destroy(&consumer);
    new_queue();
    check(eglInitialize(d, nullptr, nullptr), "reinitialize display");
    check(eglChooseConfig(d, attrs, cfgs, 128, &n) && n > 0, "choose config in fresh generation");
    cfg = nullptr;
    for (int i = 0; i < n; i++) {
        EGLint visual = 0;
        if (eglGetConfigAttrib(d, cfgs[i], EGL_NATIVE_VISUAL_ID, &visual) && visual == 0x34325241) {
            cfg = cfgs[i];
            break;
        }
    }
    check(cfg, "fresh AR24 config");
    context = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxattrs);
    win = eglCreateWindowSurface(d, cfg, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    check(context != EGL_NO_CONTEXT && win != EGL_NO_SURFACE && eglMakeCurrent(d, win, win, context),
          "bind fresh surface after termination");
    EGLint fresh_age = -1;
    check(eglQuerySurface(d, win, EGL_BUFFER_AGE_EXT, &fresh_age) && fresh_age == 0,
          "no stale buffer age across display reinitialization");
    check(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "unbind fresh surface");
    printf("CYCLE_REINITIALIZED fresh_age=%d\n", fresh_age);
    check(eglDestroySurface(d, win) && eglDestroyContext(d, context) && eglTerminate(d), "release EGL");
    check(!unref(window), "release producer");
    destroy(&consumer);
    printf("SCANOUT_CYCLE_PASS frames=%d distinct_bos=%zu\n", frame_count, buffers.size());
}
