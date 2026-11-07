/* Mock NativeWindow queue + real allocator/DRM/Mesa. Original queue integration
 * is a separate gate; this fixture validates WSI mechanics and failures. */
#include "idisplay_buffer_vdi.h"
#include "display_type.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/eventfd.h>
#include <unistd.h>
using Vdi = OHOS::HDI::Display::Buffer::V1_0::IDisplayBufferVdi;
static Vdi *allocator;
static void check(bool value, const char *what)
{ if (!value) { fprintf(stderr, "EGL_WINDOW_PROBE_FAIL %s EGL=0x%x GL=0x%x\n", what, eglGetError(), glGetError()); exit(2); } }
struct NativeWindow { int width = 13, height = 7, refs = 0, requests = 0, flushes = 0, aborts = 0;
    uint64_t usage = 0; bool geometry_ready = false, protected_buffer = false, fail_flush = false, fail_request = false; };
struct NativeBuffer { BufferHandle *handle; };
struct Region { struct Rect { int32_t x,y; uint32_t w,h; } *rects; int32_t count; };
extern "C" int OH_NativeWindow_NativeObjectReference(void *object) { ++static_cast<NativeWindow *>(object)->refs; return 0; }
extern "C" int OH_NativeWindow_NativeObjectUnreference(void *object) { --static_cast<NativeWindow *>(object)->refs; return 0; }
extern "C" BufferHandle *OH_NativeWindow_GetBufferHandleFromNative(NativeBuffer *object) { return object ? object->handle : nullptr; }
extern "C" int OH_NativeWindow_NativeWindowHandleOpt(NativeWindow *window, int code, ...)
{
    va_list args; va_start(args, code); int result = 0;
    if (code == 1) { *va_arg(args, int *) = window->geometry_ready ? window->height : 0;
        *va_arg(args, int *) = window->geometry_ready ? window->width : 0; }
    else if (code == 4) *va_arg(args, uint64_t *) = window->usage;
    else if (code == 5) window->usage = va_arg(args, uint64_t);
    else result = -1;
    va_end(args); return result;
}
extern "C" int OH_NativeWindow_NativeWindowRequestBuffer(NativeWindow *window, NativeBuffer **out, int *fence)
{
    *out = nullptr; *fence = -1;
    if (window->fail_request) return -1;
    auto format = window->requests % 2 ? PIXEL_FMT_BGRA_8888 : PIXEL_FMT_RGBA_8888;
    OHOS::HDI::Display::Buffer::V1_0::AllocInfo info = {uint32_t(window->width), uint32_t(window->height),
        HBM_USE_CPU_READ | HBM_USE_CPU_WRITE, uint32_t(format), 0};
    auto buffer = new NativeBuffer{};
    if (allocator->AllocMem(info, buffer->handle)) { delete buffer; return -1; }
    if (window->protected_buffer) buffer->handle->usage |= HBM_USE_PROTECTED;
    *out = buffer; *fence = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
    check(*fence >= 0, "acquire fence fixture");
    ++window->requests; return 0;
}
extern "C" int OH_NativeWindow_NativeWindowAbortBuffer(NativeWindow *window, NativeBuffer *buffer)
{ ++window->aborts; allocator->FreeMem(*buffer->handle); delete buffer; return 0; }
static int rendered_width = 13, rendered_height = 7;
extern "C" int OH_NativeWindow_NativeWindowFlushBuffer(NativeWindow *window, NativeBuffer *buffer, int fence, Region)
{
    check(fence == -1, "synchronous release fence");
    if (window->fail_flush) return -1;
    auto handle = buffer->handle;
    auto pixels = static_cast<const unsigned char *>(allocator->Mmap(*handle));
    check(pixels && !allocator->InvalidateCache(*handle), "consumer mapping");
    bool rgba = handle->format == PIXEL_FMT_RGBA_8888;
    for (int y = 0; y < handle->height; ++y) for (int x = 0; x < handle->width; ++x) {
        // GL bottom half is red; NativeWindow rows start at the top.
        bool red = rendered_height - 1 - uint64_t(y) * rendered_height / handle->height < rendered_height / 2;
        auto pixel = pixels + y * handle->stride + x * 4;
        check(pixel[rgba ? 0 : 2] == (red ? 255 : 0) && pixel[1] == (red ? 0 : 255) &&
              pixel[rgba ? 2 : 0] == 64 && pixel[3] == 255, "queue pixel orientation/format/scaling");
    }
    check(!allocator->Unmap(*handle), "consumer unmap");
    ++window->flushes;
    allocator->FreeMem(*handle); delete buffer; return 0;
}
static void render(int width, int height)
{
    rendered_width = width; rendered_height = height;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST); glClearColor(0, 1, 64.0f/255, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, width, height/2);
    glClearColor(1, 0, 64.0f/255, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
    check(glGetError() == GL_NO_ERROR, "render window default framebuffer");
}
int main()
{
    setbuf(stdout, nullptr); alarm(45);
    void *library = dlopen("/vendor/lib64/aurora-graphics/libdisplay_buffer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    check(library, "allocator library");
    auto create = reinterpret_cast<Vdi *(*)()>(dlsym(library, "CreateDisplayBufferVdi"));
    auto destroy = reinterpret_cast<void (*)(Vdi *)>(dlsym(library, "DestroyDisplayBufferVdi"));
    check(create && destroy && (allocator = create()), "allocator factory");
    EGLDisplay display = eglGetPlatformDisplayEXT(0x34e0, nullptr, nullptr);
    check(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "OHOS GBM EGL display");
    check(strstr(eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_KHR_platform_ohos") &&
          strstr(eglQueryString(display, EGL_EXTENSIONS), "EGL_OHOS_image_native_buffer"), "OHOS extension strings");
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    check(eglChooseConfig(display, attributes, &config, 1, &count) && count, "window config");
    EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    NativeWindow window;
    EGLSurface surface = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(&window), nullptr);
    check(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && window.refs == 1, "native ref and GBM backing");
    auto platform_window = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEPROC>(eglGetProcAddress("eglCreatePlatformWindowSurface"));
    check(platform_window, "EGL 1.5 platform window dispatch");
    EGLSurface second = platform_window(display, config, &window, nullptr);
    check(second != EGL_NO_SURFACE && window.refs == 2 && eglDestroySurface(display, second) && window.refs == 1,
          "EGL 1.5 platform window lifetime");
    // Original RS creates the EGL surface, then sets producer dimensions,
    // then makes it current. Also covers destroying a never-realized surface.
    window.geometry_ready = true;
    check(eglMakeCurrent(display, surface, surface, context) && eglGetCurrentSurface(EGL_DRAW) == surface, "virtual current surface");
    printf("EGL_WINDOW_RENDERER %s\n", glGetString(GL_RENDERER));
    for (int frame = 0; frame < 2; ++frame) {
        render(13, 7); check(eglSwapBuffers(display, surface), "window swap");
    }
    // ES3 application has a nondefault read FBO and a pixel-pack buffer.
    GLuint fbo, pbo;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo); glBufferData(GL_PIXEL_PACK_BUFFER, 1024, nullptr, GL_STATIC_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 8); glPixelStorei(GL_PACK_ROW_LENGTH, 17);
    glPixelStorei(GL_PACK_SKIP_ROWS, 2); glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
    render(13, 7); check(eglSwapBuffers(display, surface), "swap with app pack/PBO/FBO state");
    GLint value;
#define STATE(name, expected) glGetIntegerv(name, &value); check(value == int(expected), #name)
    STATE(GL_READ_FRAMEBUFFER_BINDING, fbo); STATE(GL_PIXEL_PACK_BUFFER_BINDING, pbo);
    STATE(GL_PACK_ALIGNMENT, 8); STATE(GL_PACK_ROW_LENGTH, 17); STATE(GL_PACK_SKIP_ROWS, 2); STATE(GL_PACK_SKIP_PIXELS, 3);
#undef STATE
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glDeleteBuffers(1, &pbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    render(13, 7); window.width = 19; window.height = 9;
    check(eglSwapBuffers(display, surface), "resize transition scaling");
    EGLint width, height;
    check(eglQuerySurface(display, surface, EGL_WIDTH, &width) && width == 19 &&
          eglQuerySurface(display, surface, EGL_HEIGHT, &height) && height == 9, "resized dimensions");
    render(19, 9); check(eglSwapBuffers(display, surface), "resized backing frame");
    window.protected_buffer = true;
    check(!eglSwapBuffers(display, surface) && eglGetError() == EGL_BAD_ALLOC && window.aborts == 1, "protected buffer aborted");
    window.protected_buffer = false; window.fail_flush = true;
    check(!eglSwapBuffers(display, surface) && eglGetError() == EGL_BAD_NATIVE_WINDOW && window.aborts == 2, "flush failure aborted");
    window.fail_flush = false; window.fail_request = true;
    check(!eglSwapBuffers(display, surface) && eglGetError() == EGL_BAD_ALLOC && window.aborts == 2, "request failure handled");
    window.fail_request = false;
    render(19, 9); check(eglSwapBuffers(display, surface), "recovery after queue errors");
    // Exercise OHOS EGL_NATIVE_BUFFER -> DMA-BUF -> Mesa image, not just the
    // window copy path. The image must retain backing storage after FD close.
    OHOS::HDI::Display::Buffer::V1_0::AllocInfo image_info = {4, 3,
        HBM_USE_CPU_READ | HBM_USE_CPU_WRITE, PIXEL_FMT_RGBA_8888, 0};
    NativeBuffer image_buffer{};
    check(!allocator->AllocMem(image_info, image_buffer.handle), "native image buffer");
    auto image_pixels = static_cast<unsigned char *>(allocator->Mmap(*image_buffer.handle));
    check(image_pixels, "native image CPU mapping");
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) {
        auto pixel = image_pixels + y * image_buffer.handle->stride + x*4;
        pixel[0] = 17; pixel[1] = 51; pixel[2] = 85; pixel[3] = 255;
    }
    check(!allocator->FlushCache(*image_buffer.handle) && !allocator->Unmap(*image_buffer.handle), "publish native image");
    auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto target_image = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    check(create_image && destroy_image && target_image, "image extension dispatch");
    EGLImageKHR image = create_image(display, EGL_NO_CONTEXT, 0x34e1, &image_buffer, nullptr);
    check(image != EGL_NO_IMAGE_KHR, "OHOS native image import");
    auto core_create_image = reinterpret_cast<PFNEGLCREATEIMAGEPROC>(eglGetProcAddress("eglCreateImage"));
    auto core_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEPROC>(eglGetProcAddress("eglDestroyImage"));
    check(core_create_image && core_destroy_image, "EGL 1.5 native image dispatch");
    EGLAttrib preserved[] = {EGL_IMAGE_PRESERVED, EGL_TRUE, EGL_NONE};
    EGLImage second_image = core_create_image(display, EGL_NO_CONTEXT, 0x34e1, &image_buffer, preserved);
    check(second_image != EGL_NO_IMAGE && core_destroy_image(display, second_image), "EGL 1.5 native image import");
    allocator->FreeMem(*image_buffer.handle); image_buffer.handle = nullptr;
    GLuint texture, image_fbo;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    target_image(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &image_fbo); glBindFramebuffer(GL_FRAMEBUFFER, image_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "native image FBO");
    unsigned char sample[4]{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    check(glGetError() == GL_NO_ERROR && sample[0] == 17 && sample[1] == 51 && sample[2] == 85 && sample[3] == 255,
          "native image pixels after allocator free");
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &image_fbo); glDeleteTextures(1, &texture);
    check(destroy_image(display, image), "native image cleanup");
    puts("EGL_NATIVE_BUFFER_IMPORT_PASS rgba=17,51,85,255 closed-origin-fd");
    check(eglDestroySurface(display, surface) && window.refs == 1, "destroy current surface deferred native release");
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) && window.refs == 0, "unbind releases native window");
    check(eglDestroyContext(display, context) && eglTerminate(display), "EGL cleanup");
    destroy(allocator); dlclose(library);
    printf("EGL_WINDOW_BRIDGE_PASS flushes=%d aborts=%d refs=%d orientation/format/resize/pack-state/failure/lifetime\n",
           window.flushes, window.aborts, window.refs);
    return 0;
}
