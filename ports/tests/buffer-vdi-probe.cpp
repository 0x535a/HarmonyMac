/* Uses the public VDI call shape and the independently verified fixed handle ABI. */
#include "idisplay_buffer_vdi.h"
#include "aurora_drm.h"
#include "display_type.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <unistd.h>

using BufferInterface = OHOS::HDI::Display::Buffer::V1_0::IDisplayBufferVdi;
using BufferAlloc = OHOS::HDI::Display::Buffer::V1_0::AllocInfo;
using BufferVerify = OHOS::HDI::Display::Buffer::V1_0::VerifyAllocInfo;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "BUFFER_VDI_PROBE_FAIL %s EGL=0x%x\n", what, eglGetError());
        std::exit(2);
    }
}

int main()
{
    setbuf(stdout, nullptr);
    alarm(30);
    void *library = dlopen("/vendor/lib64/aurora-graphics/libdisplay_buffer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::puts(dlerror()); return 2; }
    auto create = reinterpret_cast<BufferInterface *(*)()>(dlsym(library, "CreateDisplayBufferVdi"));
    auto destroy = reinterpret_cast<void (*)(BufferInterface *)>(dlsym(library, "DestroyDisplayBufferVdi"));
    check(create && destroy, "factory symbols");
    BufferInterface *allocator = create();
    check(allocator, "factory and fixed handle ABI");
    std::vector<BufferVerify> requests = {{13, 7, HBM_USE_CPU_READ | HBM_USE_CPU_WRITE, PIXEL_FMT_BGRA_8888},
        {13, 7, HBM_USE_PROTECTED, PIXEL_FMT_BGRA_8888}};
    std::vector<bool> supported;
    check(!allocator->IsSupportedAlloc(requests, supported) && supported.size() == 2 &&
          supported[0] && !supported[1], "std::__h vector<bool> and protected rejection");
    AuroraDrmDisplay drm = {};
    check(!AuroraOpenDisplayDrm(&drm, 0), "DRM display enumeration");
    gbm_device *device = gbm_create_device(drm.fd);
    check(device, "GBM device");
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    check(platform, "EGL GBM entry point");
    EGLDisplay display = platform(EGL_PLATFORM_GBM_KHR, device, nullptr);
    check(eglInitialize(display, nullptr, nullptr), "GBM EGL initialize");
    // GBM need not expose pbuffers. All rendering below targets imported FBOs.
    EGLint attributes[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    check(eglChooseConfig(display, attributes, &config, 1, &count) && count, "GBM config");
    EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    check(extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context"), "GBM surfaceless support");
    check(context != EGL_NO_CONTEXT &&
          eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "GBM current context");
    auto image_create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto image_destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    check(image_create && image_destroy && image_target, "DMA-BUF EGL image entry points");
    std::printf("BUFFER_VDI renderer=%s DRM=%s mode=%ux%u\n", glGetString(GL_RENDERER), drm.driver,
                drm.mode.hdisplay, drm.mode.vdisplay);
    for (int sample = 0; sample < 2; ++sample) {
        BufferAlloc info = {sample ? static_cast<uint32_t>(drm.mode.hdisplay) : 13U,
                            sample ? static_cast<uint32_t>(drm.mode.vdisplay) : 7U,
                            HBM_USE_CPU_READ | HBM_USE_CPU_WRITE | HBM_USE_HW_RENDER,
                            PIXEL_FMT_BGRA_8888, 0};
        BufferHandle *handle = nullptr;
        check(!allocator->AllocMem(info, handle) && handle, "linear DRM allocation");
        check(handle->width == static_cast<int>(info.width) && handle->height == static_cast<int>(info.height), "geometry");
        check(!allocator->RegisterBuffer(*handle), "register clone-compatible handle");
        auto pixels = static_cast<unsigned char *>(allocator->Mmap(*handle));
        check(pixels, "DMA-BUF CPU map");
        check(handle->virAddr == pixels, "stock HDI mapped address contract");
        for (int y = 0; y < handle->height; ++y) {
            for (int x = 0; x < handle->width; ++x) {
                auto p = pixels + y * handle->stride + x * 4;
                p[0] = 191; p[1] = 128; p[2] = 64; p[3] = 255;
            }
        }
        check(!allocator->FlushCache(*handle), "CPU cache publication");
        std::vector<uint8_t> metadata = {1, 2, 3, 4}, returned;
        check(!allocator->SetMetadata(*handle, 17, metadata), "metadata set");
        check(!allocator->GetMetadata(*handle, 17, returned) && returned == metadata, "metadata vector ABI");
        OHOS::HDI::Display::Buffer::V1_2::ImageLayout layout;
        check(!allocator->GetImageLayout(*handle, layout) && layout.planes.size() == 1 &&
              layout.planes[0].vStride == static_cast<uint32_t>(handle->stride), "linear image layout");
        check(!allocator->Unmap(*handle), "CPU unmap");
        check(!handle->virAddr, "stock HDI unmapped address contract");
        const EGLint image_attributes[] = {EGL_WIDTH, handle->width, EGL_HEIGHT, handle->height,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, handle->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, handle->stride, EGL_NONE};
        EGLImageKHR image = image_create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, image_attributes);
        check(image != EGL_NO_IMAGE_KHR, "DRM FD EGL import");
        GLuint texture, fbo;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        image_target(GL_TEXTURE_2D, image);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "imported FBO");
        unsigned char result[4];
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, result);
        check(glGetError() == GL_NO_ERROR && result[0] == 64 && result[1] == 128 &&
              result[2] == 191 && result[3] == 255, "imported pixel content");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        image_destroy(display, image);
        std::printf("BUFFER_VDI_IMPORT_PASS sample=%d size=%dx%d stride=%d bytes=%d\n", sample,
                    handle->width, handle->height, handle->stride, handle->size);
        if (sample) {
            check(!drmSetMaster(drm.fd), "KMS master");
            uint32_t gem, fb, handles[4] = {}, strides[4] = {}, offsets[4] = {};
            check(!drmPrimeFDToHandle(drm.fd, handle->fd, &gem), "scanout PRIME import");
            handles[0] = gem; strides[0] = handle->stride;
            check(!drmModeAddFB2(drm.fd, handle->width, handle->height, DRM_FORMAT_XRGB8888,
                               handles, strides, offsets, &fb, 0), "scanout framebuffer");
            check(!drmModeSetCrtc(drm.fd, drm.crtc, fb, 0, 0, &drm.connector, 1, &drm.mode), "KMS scanout commit");
            puts("BUFFER_VDI_KMS_VISIBLE rgba=64,128,191,255");
            sleep(10);
        }
        allocator->FreeMem(*handle);
    }
    puts("BUFFER_VDI_FULL_PROBE_PASS");
    destroy(allocator);
    return 0;
}
