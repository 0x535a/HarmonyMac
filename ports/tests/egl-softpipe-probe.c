#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DRAW_RECORD_WORKLOAD
#include <time.h>
#endif
#ifdef DYNAMIC_MESA
#include <dlfcn.h>
#define DECLARE(name) static __typeof__(&name) dynamic_##name
DECLARE(eglGetProcAddress); DECLARE(eglInitialize); DECLARE(eglGetError);
DECLARE(eglChooseConfig); DECLARE(eglCreateContext); DECLARE(eglCreatePbufferSurface);
DECLARE(eglMakeCurrent); DECLARE(eglDestroySurface); DECLARE(eglDestroyContext); DECLARE(eglTerminate);
DECLARE(glGetString); DECLARE(glViewport); DECLARE(glClearColor); DECLARE(glClear);
DECLARE(glReadPixels); DECLARE(glFinish); DECLARE(glGetError);
#ifdef BUFFER_RELEASE_TEST
DECLARE(glGenBuffers); DECLARE(glBindBuffer); DECLARE(glBufferData);
DECLARE(glMapBufferRange); DECLARE(glUnmapBuffer); DECLARE(glDeleteBuffers);
#endif
#ifdef DRAW_RECORD_TEST
DECLARE(glCreateShader); DECLARE(glShaderSource); DECLARE(glCompileShader);
DECLARE(glGetShaderiv); DECLARE(glCreateProgram); DECLARE(glAttachShader);
DECLARE(glLinkProgram); DECLARE(glGetProgramiv); DECLARE(glUseProgram);
DECLARE(glDrawArrays); DECLARE(glDeleteShader); DECLARE(glDeleteProgram);
#endif
static int load_dynamic(void)
{
    void *egl = dlopen("/system/lib64/aurora-mesa/libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void *gles = dlopen("/system/lib64/aurora-mesa/libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gles) { printf("MESA_DYNAMIC_LOAD_FAIL %s\n", dlerror()); return 1; }
#define LOAD(name, lib) do { dynamic_##name = (__typeof__(dynamic_##name))dlsym(lib, #name); if (!dynamic_##name) return 1; } while (0)
    LOAD(eglGetProcAddress, egl); LOAD(eglInitialize, egl); LOAD(eglGetError, egl);
    LOAD(eglChooseConfig, egl); LOAD(eglCreateContext, egl); LOAD(eglCreatePbufferSurface, egl);
    LOAD(eglMakeCurrent, egl); LOAD(eglDestroySurface, egl); LOAD(eglDestroyContext, egl); LOAD(eglTerminate, egl);
    LOAD(glGetString, gles); LOAD(glViewport, gles); LOAD(glClearColor, gles); LOAD(glClear, gles);
    LOAD(glReadPixels, gles); LOAD(glFinish, gles); LOAD(glGetError, gles);
#ifdef BUFFER_RELEASE_TEST
    LOAD(glGenBuffers, gles); LOAD(glBindBuffer, gles); LOAD(glBufferData, gles);
    LOAD(glMapBufferRange, gles); LOAD(glUnmapBuffer, gles); LOAD(glDeleteBuffers, gles);
#endif
#ifdef DRAW_RECORD_TEST
    LOAD(glCreateShader, gles); LOAD(glShaderSource, gles); LOAD(glCompileShader, gles);
    LOAD(glGetShaderiv, gles); LOAD(glCreateProgram, gles); LOAD(glAttachShader, gles);
    LOAD(glLinkProgram, gles); LOAD(glGetProgramiv, gles); LOAD(glUseProgram, gles);
    LOAD(glDrawArrays, gles); LOAD(glDeleteShader, gles); LOAD(glDeleteProgram, gles);
#endif
#undef LOAD
    return 0;
}
#define eglGetProcAddress dynamic_eglGetProcAddress
#define eglInitialize dynamic_eglInitialize
#define eglGetError dynamic_eglGetError
#define eglChooseConfig dynamic_eglChooseConfig
#define eglCreateContext dynamic_eglCreateContext
#define eglCreatePbufferSurface dynamic_eglCreatePbufferSurface
#define eglMakeCurrent dynamic_eglMakeCurrent
#define eglDestroySurface dynamic_eglDestroySurface
#define eglDestroyContext dynamic_eglDestroyContext
#define eglTerminate dynamic_eglTerminate
#define glGetString dynamic_glGetString
#define glViewport dynamic_glViewport
#define glClearColor dynamic_glClearColor
#define glClear dynamic_glClear
#define glReadPixels dynamic_glReadPixels
#define glFinish dynamic_glFinish
#define glGetError dynamic_glGetError
#ifdef BUFFER_RELEASE_TEST
#define glGenBuffers dynamic_glGenBuffers
#define glBindBuffer dynamic_glBindBuffer
#define glBufferData dynamic_glBufferData
#define glMapBufferRange dynamic_glMapBufferRange
#define glUnmapBuffer dynamic_glUnmapBuffer
#define glDeleteBuffers dynamic_glDeleteBuffers
#endif
#ifdef DRAW_RECORD_TEST
#define glCreateShader dynamic_glCreateShader
#define glShaderSource dynamic_glShaderSource
#define glCompileShader dynamic_glCompileShader
#define glGetShaderiv dynamic_glGetShaderiv
#define glCreateProgram dynamic_glCreateProgram
#define glAttachShader dynamic_glAttachShader
#define glLinkProgram dynamic_glLinkProgram
#define glGetProgramiv dynamic_glGetProgramiv
#define glUseProgram dynamic_glUseProgram
#define glDrawArrays dynamic_glDrawArrays
#define glDeleteShader dynamic_glDeleteShader
#define glDeleteProgram dynamic_glDeleteProgram
#endif
#endif

int main(void)
{
    setbuf(stdout, NULL);
#ifdef DYNAMIC_MESA
    if (load_dynamic()) return 1;
#endif
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform) return 2;
    EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        printf("EGL_INITIALIZE_FAIL 0x%x\n", eglGetError());
        return 3;
    }
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count) return 4;
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
#ifdef DRAW_RECORD_WORKLOAD
    const EGLint surface_attributes[] = {EGL_WIDTH, 420, EGL_HEIGHT, 420, EGL_NONE};
#else
    const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
#endif
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) return 5;
    printf("EGL %d.%d renderer=%s GLES=%s\n", major, minor,
           glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glViewport(0, 0, 16, 16);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixels[16 * 16 * 4];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glFinish();
    if (glGetError() != GL_NO_ERROR) return 6;
    for (unsigned int i = 0; i < sizeof(pixels); i += 4) {
        if (abs((int)pixels[i] - 64) > 1 || abs((int)pixels[i+1] - 128) > 1 ||
            abs((int)pixels[i+2] - 191) > 1 || pixels[i+3] != 255) {
            printf("PIXEL_FAIL at=%u rgba=%u,%u,%u,%u\n", i/4,
                   pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
            return 7;
        }
    }
    printf("OHOS_SOFTPIPE_RENDER_PASS 256 pixels rgba=%u,%u,%u,%u\n",
           pixels[0], pixels[1], pixels[2], pixels[3]);
#ifdef BUFFER_RELEASE_TEST
    GLuint buffer;
    unsigned char expected[4096];
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    for (unsigned i = 1; i <= 16; ++i) {
        size_t size = i * 256;
        memset(expected, (int)i, size);
        glBufferData(GL_ARRAY_BUFFER, size, expected, GL_DYNAMIC_DRAW);
        void *mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, size, GL_MAP_READ_BIT);
        if (!mapped || memcmp(mapped, expected, size)) return 8;
        if (!glUnmapBuffer(GL_ARRAY_BUFFER) || glGetError() != GL_NO_ERROR) return 9;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    glFinish();
    if (glGetError() != GL_NO_ERROR) return 10;
    puts("OHOS_DDEBUG_BUFFER_RELEASE_PASS reallocations=16");
#endif
#ifdef DRAW_RECORD_TEST
    const char *vertex = "#version 300 es\nvoid main(){ vec2 p[3]=vec2[3](vec2(-1.,-1.),vec2(3.,-1.),vec2(-1.,3.)); gl_Position=vec4(p[gl_VertexID],0.,1.); }";
    const char *fragment = "#version 300 es\nprecision highp float; out vec4 color; void main(){ color=vec4(.25,.5,.75,1.); }";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vertex, NULL); glCompileShader(vs);
    glShaderSource(fs, 1, &fragment, NULL); glCompileShader(fs);
    GLint valid;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &valid); if (!valid) return 11;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &valid); if (!valid) return 12;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &valid); if (!valid) return 13;
    glUseProgram(program);
    for (unsigned i = 0; i < 100; ++i) glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glFinish();
    if (glGetError() != GL_NO_ERROR) return 14;
    for (unsigned i = 0; i < sizeof(pixels); i += 4)
        if (abs((int)pixels[i]-64)>1 || abs((int)pixels[i+1]-128)>1 ||
            abs((int)pixels[i+2]-191)>1 || pixels[i+3]!=255) return 15;
    glUseProgram(0); glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    puts("OHOS_DDEBUG_DRAW_RECORD_PASS calls=100 pixels=256");
#endif
#ifdef DRAW_RECORD_WORKLOAD
    /* Analytic AA coverage, derivatives and many real full-surface draws.
     * Far from the boundary the expected pixels are exact; edge pixels are
     * excluded from this check because derivative rounding is backend-specific.
     */
    const char *work_vs = "#version 300 es\nvoid main(){ vec2 p[3]=vec2[3](vec2(-1.,-1.),vec2(3.,-1.),vec2(-1.,3.)); gl_Position=vec4(p[gl_VertexID],0.,1.); }";
    const char *work_fs = "#version 300 es\nprecision highp float; out vec4 color; void main(){ vec2 p=gl_FragCoord.xy*(2./420.)-1.; float d=p.x*p.x-p.y; float grad=length(vec2(dFdx(d),dFdy(d))); float a=clamp(.5-d/max(grad,.000001),0.,1.); color=vec4(.25,.5,.75,1.)*a; }";
    GLuint wvs = glCreateShader(GL_VERTEX_SHADER), wfs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(wvs, 1, &work_vs, NULL); glCompileShader(wvs);
    glShaderSource(wfs, 1, &work_fs, NULL); glCompileShader(wfs);
    GLint compiled;
    glGetShaderiv(wvs, GL_COMPILE_STATUS, &compiled); if (!compiled) return 16;
    glGetShaderiv(wfs, GL_COMPILE_STATUS, &compiled); if (!compiled) return 17;
    GLuint wp = glCreateProgram();
    glAttachShader(wp, wvs); glAttachShader(wp, wfs); glLinkProgram(wp);
    glGetProgramiv(wp, GL_LINK_STATUS, &compiled); if (!compiled) return 18;
    glUseProgram(wp); glViewport(0, 0, 420, 420);
    glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (unsigned i = 0; i < 128; i++) glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    clock_gettime(CLOCK_MONOTONIC, &end);
    unsigned char *image = malloc(420 * 420 * 4);
    if (!image) return 19;
    glReadPixels(0, 0, 420, 420, GL_RGBA, GL_UNSIGNED_BYTE, image);
    if (glGetError() != GL_NO_ERROR) return 20;
    unsigned checked = 0;
    for (unsigned y = 0; y < 420; y++) {
        for (unsigned x = 0; x < 420; x++) {
            float px = ((float)x + .5f) * (2.f / 420.f) - 1.f;
            float py = ((float)y + .5f) * (2.f / 420.f) - 1.f;
            float diff = px * px - py;
            if (diff > -.03f && diff < .03f) continue;
            const unsigned char filled[] = {64, 128, 191, 255};
            for (unsigned c = 0; c < 4; c++) {
                int expected = diff < 0 ? filled[c] : 0;
                if (abs((int)image[(y * 420 + x) * 4 + c] - expected) > 1) {
                    printf("GLES_WORKLOAD_PIXEL_FAIL x=%u y=%u channel=%u actual=%u expected=%d\n",
                           x, y, c, image[(y * 420 + x) * 4 + c], expected);
                    free(image); return 21;
                }
            }
            checked++;
        }
    }
    free(image);
    long long elapsed = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec - start.tv_nsec;
    printf("OHOS_GLES_WORKLOAD_PASS draws=128 size=420x420 checked_pixels=%u driver_ns=%lld\n", checked, elapsed);
    glUseProgram(0); glDeleteProgram(wp); glDeleteShader(wvs); glDeleteShader(wfs);
#endif
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
