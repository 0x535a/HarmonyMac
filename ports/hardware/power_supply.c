/* SPDX-License-Identifier: GPL-3.0-only */
#include "power_supply.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef AURORA_POWER_SUPPLY_ROOT
#define AURORA_POWER_SUPPLY_ROOT "/sys/class/power_supply"
#endif

int AuroraFindBattery(char *path, size_t size)
{
    if (!path || !size) { errno = EINVAL; return -1; }
    DIR *root = opendir(AURORA_POWER_SUPPLY_ROOT);
    if (!root) return -1;
    struct dirent *entry;
    char selected[NAME_MAX+1] = {0};
    int count = 0, failure = ENODEV;
    while ((entry = readdir(root))) {
        if (entry->d_name[0] == '.') continue;
        int dir = openat(dirfd(root), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) { failure = errno; continue; }
        int fd = openat(dir, "type", O_RDONLY | O_CLOEXEC);
        close(dir);
        if (fd < 0) { failure = errno; continue; }
        char type[32];
        ssize_t n;
        do { n = read(fd, type, sizeof(type)-1); } while (n < 0 && errno == EINTR);
        int saved = errno;
        close(fd);
        if (n < 0) { failure = saved; continue; }
        type[n] = 0;
        while (n && (type[n-1] == '\n' || type[n-1] == '\r')) type[--n] = 0;
        if (strcmp(type, "Battery")) continue;
        // Do not confuse a Bluetooth HID peripheral's battery with the laptop.
        dir = openat(dirfd(root), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        fd = dir < 0 ? -1 : openat(dir, "scope", O_RDONLY | O_CLOEXEC);
        if (dir >= 0) close(dir);
        if (fd >= 0) {
            do { n = read(fd, type, sizeof(type)-1); } while (n < 0 && errno == EINTR);
            saved = errno; close(fd);
            if (n < 0) { failure = saved; continue; }
            type[n] = 0;
            if (!strncmp(type, "Device", 6) && (type[6] == 0 || type[6] == '\n')) continue;
        }
        if (strlen(entry->d_name) > NAME_MAX) { failure = ENAMETOOLONG; continue; }
        strcpy(selected, entry->d_name);
        ++count;
    }
    closedir(root);
    if (count != 1) { errno = count > 1 ? EBUSY : (failure == ENOENT ? ENODEV : failure); return -1; }
    int len = snprintf(path, size, "%s/%s", AURORA_POWER_SUPPLY_ROOT, selected);
    if (len < 0 || (size_t)len >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

const char *AuroraBatteryRoot(void)
{
    static _Thread_local char path[PATH_MAX];
    return AuroraFindBattery(path, sizeof(path)) ? NULL : path;
}
