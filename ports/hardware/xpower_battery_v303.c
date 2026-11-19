/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Only the XPower DSO's renamed imports call these entry points. No global
 * open/ioctl interposition; no /dev node and no fabricated power history.
 * ABI: shipped 0x1638f8 -> 0x161f74, _IOWR(4,1,32-byte union).
 * Properties 0..9: local HM dubai_battery_stats.c. 10: full capacity uAh
 * (0x1ff020 writes battery_full_capacity after /1000); 12: remaining charge
 * uAh (0x1ff808 and 0x1ff918 use its /1000 value for charge-drain accounting);
 * 13: design capacity mAh (0x206470, DESIGNCAPACITY vs FULLCAPACITY).
 * Property 11 is not implemented without an established contract.
 */
#ifndef L410_BATTERY_ROOT
#define L410_BATTERY_ROOT "/sys/class/power_supply/Battery"
#endif
#define BATTERY_IOC 0xc0200401UL
union battery_value { int32_t value; char text[32]; };
_Static_assert(sizeof(union battery_value) == 32, "Dubai ABI");

static int read_property(const char *name, char *buf, size_t len)
{
    char path[256];
    const char *root = L410_BATTERY_ROOT;
    if (!root) return -1;
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, buf, len - 1); } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0) { errno = n < 0 ? saved : ENODATA; return -1; }
    if ((size_t)n == len - 1) { errno = EOVERFLOW; return -1; }
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) --n;
    buf[n] = 0;
    return 0;
}

static int number_property(const char *name, int divisor, int32_t *out)
{
    char buf[128], *end;
    if (read_property(name, buf, sizeof(buf))) return -1;
    errno = 0;
    long long value = strtoll(buf, &end, 10);
    if (errno || end == buf || *end) { errno = EINVAL; return -1; }
    value /= divisor;
    if (value < INT32_MIN || value > INT32_MAX) { errno = ERANGE; return -1; }
    *out = (int32_t)value;
    return 0;
}

int l410_xpower_battery_value(union battery_value *arg)
{
    if (!arg) { errno = EFAULT; return -1; }
    union battery_value result = { .text = {0} };
    const char *name = NULL;
    int divisor = 1;
    switch (arg->value) {
    case 0: name = "charge_now"; divisor = 1000; break;
    case 1: name = "voltage_now"; divisor = 1000; break;
    case 2: name = "current_now"; break;
    case 3: name = "cycle_count"; break;
    case 4: name = "charge_full"; divisor = 1000; break;
    case 5: name = "capacity"; break;
    case 6: /* Same explicit unknown-brand representation as stock Dubai. */
        if (read_property("manufacturer", result.text, sizeof(result.text))) {
            if (errno != ENOENT && errno != ENODATA) return -1;
            memcpy(result.text, "Unknown", 8);
        }
        break;
    case 7: {
        char text[128];
        if (read_property("status", text, sizeof(text))) return -1;
        if (strcmp(text, "Charging") == 0 || strcmp(text, "Full") == 0) result.value = 1;
        else if (strcmp(text, "Discharging") && strcmp(text, "Not charging") && strcmp(text, "Unknown")) {
            errno = EINVAL; return -1;
        }
        break;
    }
    case 8: {
        /* Linux power_supply HEALTH enumeration, not Harmony BatteryHealth. */
        static const char *const health[] = {"Unknown", "Good", "Overheat", "Dead", "Over voltage",
            "Unspecified failure", "Cold", "Watchdog timer expire", "Safety timer expire"};
        char text[128];
        if (read_property("health", text, sizeof(text))) return -1;
        size_t i;
        for (i = 0; i < sizeof(health)/sizeof(health[0]); ++i) if (!strcmp(text, health[i])) break;
        if (i == sizeof(health)/sizeof(health[0])) { errno = EOPNOTSUPP; return -1; }
        result.value = (int32_t)i;
        break;
    }
    case 9: name = "temp"; break;
    case 10: name = "charge_full"; break;
    case 12: name = "charge_now"; break;
    case 13: name = "charge_full_design"; divisor = 1000; break;
    default: errno = EOPNOTSUPP; return -1;
    }
    if (name && number_property(name, divisor, &result.value)) return -1;
    memcpy(arg, &result, sizeof(result));
    return 0;
}

/* A private memfd identifies this fallback; real Dubai, if ever present, wins.
 * Device/inode checking prevents fd reuse from capturing unrelated ioctls. */
struct handle { int fd; dev_t dev; ino_t ino; struct handle *next; };
static struct handle *handles;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int l410_xpower_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    int fd = open(path, flags, mode);
    if (fd >= 0 || errno != ENOENT || !path || strcmp(path, "/dev/dubai") ||
        (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) return fd;
    fd = syscall(SYS_memfd_create, "l410-xpower-battery", 1U);
    if (fd < 0) return -1;
    struct stat st;
    struct handle *h = malloc(sizeof(*h));
    if (!h || fstat(fd, &st)) {
        int saved = h ? errno : ENOMEM;
        free(h); close(fd); errno = saved; return -1;
    }
    h->fd = fd; h->dev = st.st_dev; h->ino = st.st_ino;
    pthread_mutex_lock(&lock);
    h->next = handles; handles = h;
    pthread_mutex_unlock(&lock);
    return fd;
}

static int is_private(int fd)
{
    struct stat st;
    if (fstat(fd, &st)) return 0;
    for (struct handle *h = handles; h; h = h->next)
        if (h->fd == fd && h->dev == st.st_dev && h->ino == st.st_ino) return 1;
    return 0;
}

int l410_xpower_ioctl(int fd, unsigned long request, ...)
{
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    pthread_mutex_lock(&lock);
    int owned = is_private(fd);
    pthread_mutex_unlock(&lock);
    if (!owned) return ioctl(fd, request, arg);
    if (request != BATTERY_IOC) { errno = ENOTTY; return -1; }
    return l410_xpower_battery_value(arg);
}

int l410_xpower_close(int fd)
{
    pthread_mutex_lock(&lock);
    struct handle **p = &handles;
    while (*p) {
        if ((*p)->fd == fd) { struct handle *old = *p; *p = old->next; free(old); }
        else p = &(*p)->next;
    }
    int ret = close(fd);
    pthread_mutex_unlock(&lock);
    return ret;
}
