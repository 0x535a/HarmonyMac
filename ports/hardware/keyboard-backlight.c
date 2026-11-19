/* SPDX-License-Identifier: GPL-3.0-only
 * Linux keyboard illumination backend using standard LED class functions.
 * Select exactly one kbd_backlight device; never touch Caps Lock, fan or panel LEDs.
 */
#include "keyboard-backlight.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef AURORA_KEYBOARD_LIGHT_ROOT
#define AURORA_KEYBOARD_LIGHT_ROOT "/sys/class/leds"
#endif

static int read_number(int dir, const char *name, uint32_t *out)
{
    char buf[64], *end;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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

static int keyboard_name(const char *name)
{
    const char *function = strrchr(name, ':');
    function = function ? function+1 : name;
    const char *suffix = "kbd_backlight";
    size_t length = strlen(suffix);
    if (strncmp(function, suffix, length)) return 0;
    const char *end = function+length;
    if (!*end) return 1;
    // Linux appends _N when multiple devices request the same LED name.
    if (*end++ != '_' || *end < '0' || *end > '9') return 0;
    while (*end >= '0' && *end <= '9') ++end;
    return !*end;
}

static int open_keyboard(uint32_t *maximum)
{
    DIR *root = opendir(AURORA_KEYBOARD_LIGHT_ROOT);
    if (!root) return -1;
    int chosen = -1, count = 0, failure = ENODEV;
    struct dirent *entry;
    while ((entry = readdir(root))) {
        if (entry->d_name[0] == '.' || !keyboard_name(entry->d_name)) continue;
        ++count;
        // Linux class entries are sysfs symlinks: intentionally follow them.
        int fd = openat(dirfd(root), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { failure = errno; continue; }
        uint32_t max = 0;
        int result = read_number(fd, "max_brightness", &max);
        if (result || max == 0) {
            failure = result ? errno : EINVAL;
            close(fd); continue;
        }
        if (chosen >= 0) close(chosen);
        chosen = fd; *maximum = max;
    }
    closedir(root);
    // An ambiguous device must not adjust a different keyboard. Extend routing
    // alongside explicit keyboard routing, instead of relying on readdir order.
    if (count != 1 || chosen < 0) {
        if (chosen >= 0) close(chosen);
        errno = count > 1 ? EBUSY : failure;
        return -1;
    }
    return chosen;
}

int AuroraGetKeyboardBacklight(uint32_t *level)
{
    if (!level) { errno = EINVAL; return -1; }
    uint32_t max = 0, value = 0;
    int fd = open_keyboard(&max);
    if (fd < 0) return -1;
    int result = read_number(fd, "brightness", &value);
    int saved = errno;
    close(fd);
    if (result) { errno = saved; return -1; }
    if (value > max) { errno = ERANGE; return -1; }
    *level = (uint32_t)(((uint64_t)value * 255 + max/2) / max);
    return 0;
}

int AuroraSetKeyboardBacklight(uint32_t level)
{
    if (level > 255) { errno = EINVAL; return -1; }
    uint32_t max = 0;
    int dir = open_keyboard(&max);
    if (dir < 0) return -1;
    int fd = openat(dir, "brightness", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    int saved = errno;
    if (fd < 0) { close(dir); errno = saved; return -1; }
    char text[32];
    uint32_t requested = (uint32_t)(((uint64_t)level * max + 127)/255);
    int len = snprintf(text, sizeof(text), "%u\n", requested);
    ssize_t n;
    do { n = write(fd, text, (size_t)len); } while (n < 0 && errno == EINTR);
    saved = n < 0 ? errno : EIO;
    int closed = close(fd);
    if (n != len || closed) { if (closed && n == len) saved = errno; close(dir); errno = saved; return -1; }
    // LED blocking setters may execute on a kernel worker after the sysfs
    // write returns. A successful control operation includes applied readback.
    // Keep this device fd so hotplug cannot redirect the confirmation elsewhere.
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        uint32_t actual;
        if (read_number(dir, "brightness", &actual)) {
            saved = errno; close(dir); errno = saved; return -1;
        }
        if (actual == requested) { close(dir); return 0; }
        usleep(10000);
    }
    close(dir); errno = ETIMEDOUT; return -1;
}
