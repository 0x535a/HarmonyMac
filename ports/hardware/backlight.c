/* SPDX-License-Identifier: GPL-3.0-only
 * Linux backlight class backend, shared by OHOS composer and focused tests.
 * No L410 path, fixed Apple driver name, or permanent cached device is assumed.
 */
#include "backlight.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef AURORA_BACKLIGHT_ROOT
#define AURORA_BACKLIGHT_ROOT "/sys/class/backlight"
#endif

static int read_number(int dir, const char *name, uint32_t *out)
{
    char buf[64], *end;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, buf, sizeof(buf)-1); } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0) { errno = n ? saved : ENODATA; return -1; }
    if (n == (ssize_t)sizeof(buf)-1) { errno = EOVERFLOW; return -1; }
    buf[n] = 0;
    if (buf[0] < '0' || buf[0] > '9') { errno = EINVAL; return -1; }
    errno = 0;
    unsigned long long value = strtoull(buf, &end, 10);
    if (errno || value > UINT32_MAX) { errno = ERANGE; return -1; }
    while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') ++end;
    if (*end) { errno = EINVAL; return -1; }
    *out = (uint32_t)value;
    return 0;
}

static int open_backlight(uint32_t *maximum)
{
    DIR *root = opendir(AURORA_BACKLIGHT_ROOT);
    if (!root) return -1;
    int chosen = -1, count = 0, failure = ENODEV;
    struct dirent *entry;
    while ((entry = readdir(root))) {
        if (entry->d_name[0] == '.') continue;
        // Linux class entries are sysfs symlinks: intentionally follow them.
        int fd = openat(dirfd(root), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { failure = errno; continue; }
        uint32_t max = 0;
        int result = read_number(fd, "max_brightness", &max);
        if (result || max == 0) {
            failure = result ? errno : EINVAL;
            close(fd); continue;
        }
        ++count;
        if (chosen >= 0) close(chosen);
        chosen = fd; *maximum = max;
    }
    closedir(root);
    // An ambiguous device must not adjust a different panel. Extend routing
    // alongside multi-display support, instead of relying on readdir order.
    if (count != 1) {
        if (chosen >= 0) close(chosen);
        errno = count > 1 ? EBUSY : failure;
        return -1;
    }
    return chosen;
}

int AuroraGetBacklight(uint32_t *level)
{
    if (!level) { errno = EINVAL; return -1; }
    uint32_t max = 0, value = 0;
    int fd = open_backlight(&max);
    if (fd < 0) return -1;
    int result = read_number(fd, "brightness", &value);
    int saved = errno;
    close(fd);
    if (result) { errno = saved; return -1; }
    if (value > max) { errno = ERANGE; return -1; }
    *level = (uint32_t)(((uint64_t)value * 255 + max/2) / max);
    return 0;
}

int AuroraSetBacklight(uint32_t level)
{
    if (level > 255) { errno = EINVAL; return -1; }
    uint32_t max = 0;
    int dir = open_backlight(&max);
    if (dir < 0) return -1;
    int fd = openat(dir, "brightness", O_WRONLY | O_CLOEXEC);
    int saved = errno;
    close(dir);
    if (fd < 0) { errno = saved; return -1; }
    char text[32];
    int len = snprintf(text, sizeof(text), "%u\n", (uint32_t)(((uint64_t)level * max + 127)/255));
    ssize_t n;
    do { n = write(fd, text, (size_t)len); } while (n < 0 && errno == EINTR);
    saved = n < 0 ? errno : EIO;
    int closed = close(fd);
    if (n != len) { errno = saved; return -1; }
    return closed;
}
