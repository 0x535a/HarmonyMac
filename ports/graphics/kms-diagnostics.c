/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int log_fd = -1;
static atomic_uint rows;
static _Thread_local void (*page_flip_callback)(int, unsigned, unsigned, unsigned, void *);
static _Thread_local void (*page_flip_callback2)(int, unsigned, unsigned, unsigned, unsigned, void *);

static void record(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void record(const char *format, ...)
{
    if (log_fd < 0 || atomic_fetch_add_explicit(&rows, 1, memory_order_relaxed) >= 48000)
        return;
    int saved = errno;
    struct timespec time = {0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    char line[256];
    int offset = snprintf(line, sizeof line, "mono=%lld.%09ld tid=%ld ",
        (long long)time.tv_sec, time.tv_nsec, syscall(SYS_gettid));
    if (offset > 0 && (size_t)offset < sizeof line - 2) {
        va_list args;
        va_start(args, format);
        int length = vsnprintf(line + offset, sizeof line - offset, format, args);
        va_end(args);
        if (length >= 0 && (size_t)(length + offset) < sizeof line - 1) {
            line[length + offset] = '\n';
            (void)write(log_fd, line, length + offset + 1);
        }
    }
    errno = saved;
}

__attribute__((constructor))
static void open_log(void)
{
    const char *path = getenv("AURORA_KMS_DIAGNOSTICS_FILE");
    if (path && *path)
        log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log_fd >= 0) record("event=loaded pid=%ld max_rows=48000", (long)getpid());
}

static void *real_drm(const char *name)
{
    int saved = errno;
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        void *library = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
        if (library) {
            symbol = dlsym(library, name);
            dlclose(library);
        }
    }
    errno = symbol ? saved : ENOSYS;
    return symbol;
}

/* Cache pointers in thread-local storage so a compositor event thread and
 * command thread can resolve independently without a lock in a flip path. */
#define RESOLVE(function) \
    static _Thread_local __typeof__(&function) original; \
    if (!original) original = real_drm(#function); \
    if (!original) { record("event=symbol_missing name=" #function); return -1; }

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr request, uint32_t flags, void *data)
{
    RESOLVE(drmModeAtomicCommit);
    record("event=atomic_begin fd=%d flags=0x%x token=%p", fd, flags, data);
    int result = original(fd, request, flags, data);
    record("event=atomic_end result=%d errno=%d", result, result ? errno : 0);
    return result;
}

int drmModeAtomicAddProperty(drmModeAtomicReqPtr request, uint32_t object, uint32_t property, uint64_t value)
{
    RESOLVE(drmModeAtomicAddProperty);
    int result = original(request, object, property, value);
    record("event=atomic_property object=%u property=%u value=%llu result=%d",
        object, property, (unsigned long long)value, result);
    return result;
}

int drmPrimeFDToHandle(int fd, int dma_fd, uint32_t *handle)
{
    RESOLVE(drmPrimeFDToHandle);
    int result = original(fd, dma_fd, handle);
    int saved = errno;
    struct stat identity;
    if (log_fd >= 0 && !result && handle && !fstat(dma_fd, &identity)) {
        record("event=prime fd=%d handle=%u dev=%llu inode=%llu", fd, *handle,
            (unsigned long long)identity.st_dev, (unsigned long long)identity.st_ino);
    } else if (result) {
        record("event=prime_error fd=%d result=%d errno=%d", fd, result, saved);
    }
    errno = saved;
    return result;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t format,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    uint32_t *fb, uint32_t flags)
{
    RESOLVE(drmModeAddFB2);
    int result = original(fd, width, height, format, handles, pitches, offsets, fb, flags);
    record("event=addfb fd=%d fb=%u handle=%u width=%u height=%u format=0x%x result=%d errno=%d",
        fd, !result && fb ? *fb : 0, handles ? handles[0] : 0, width, height, format,
        result, result ? errno : 0);
    return result;
}

int drmModePageFlip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void *data)
{
    RESOLVE(drmModePageFlip);
    record("event=pageflip_begin fd=%d crtc=%u fb=%u flags=0x%x token=%p", fd, crtc, fb, flags, data);
    int result = original(fd, crtc, fb, flags, data);
    record("event=pageflip_end result=%d errno=%d", result, result ? errno : 0);
    return result;
}

int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y,
    uint32_t *connectors, int count, drmModeModeInfoPtr mode)
{
    RESOLVE(drmModeSetCrtc);
    record("event=modeset_begin fd=%d crtc=%u fb=%u width=%u height=%u",
        fd, crtc, fb, mode ? mode->hdisplay : 0, mode ? mode->vdisplay : 0);
    int result = original(fd, crtc, fb, x, y, connectors, count, mode);
    record("event=modeset_end result=%d errno=%d", result, result ? errno : 0);
    return result;
}

static void flip(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data)
{
    record("event=present fd=%d seq=%u drm_time=%u.%06u token=%p", fd, sequence, sec, usec, data);
    page_flip_callback(fd, sequence, sec, usec, data);
}

static void flip_with_crtc(int fd, unsigned sequence, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    record("event=present fd=%d seq=%u drm_time=%u.%06u crtc=%u token=%p", fd, sequence, sec, usec, crtc, data);
    page_flip_callback2(fd, sequence, sec, usec, crtc, data);
}

int drmHandleEvent(int fd, drmEventContextPtr context)
{
    RESOLVE(drmHandleEvent);
    if (log_fd < 0 || !context)
        return original(fd, context);
    const int version = context->version;
    if ((version != 2 || !context->page_flip_handler) &&
        (version != 3 || !context->page_flip_handler2))
        return original(fd, context);
    drmEventContext wrapped = {0};
    void (*previous)(int, unsigned, unsigned, unsigned, void *) = page_flip_callback;
    void (*previous2)(int, unsigned, unsigned, unsigned, unsigned, void *) = page_flip_callback2;
    if (version == 2) {
        memcpy(&wrapped, context, offsetof(drmEventContext, page_flip_handler) +
            sizeof context->page_flip_handler);
        page_flip_callback = context->page_flip_handler;
        wrapped.page_flip_handler = flip;
    } else {
        memcpy(&wrapped, context, offsetof(drmEventContext, page_flip_handler2) +
            sizeof context->page_flip_handler2);
        page_flip_callback2 = context->page_flip_handler2;
        wrapped.page_flip_handler2 = flip_with_crtc;
    }
    int result = original(fd, &wrapped);
    page_flip_callback = previous;
    page_flip_callback2 = previous2;
    if (result) record("event=handle_error result=%d errno=%d", result, errno);
    return result;
}
