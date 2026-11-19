/* SPDX-License-Identifier: Apache-2.0 */
#include "linux-thermal.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef AURORA_THERMAL_ROOT
#define AURORA_THERMAL_ROOT "/sys/class/thermal"
#endif
#ifndef AURORA_HWMON_ROOT
#define AURORA_HWMON_ROOT "/sys/class/hwmon"
#endif

namespace {
bool numbered(const char *name, const char *prefix, const char *suffix = "")
{
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || name[n] < '0' || name[n] > '9') return false;
    const char *at = name + n;
    while (*at >= '0' && *at <= '9') ++at;
    return !strcmp(at, suffix);
}

int text(int directory, const char *attribute, std::string &out)
{
    out.clear();
    int fd = openat(directory, attribute, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    char buf[256];
    ssize_t count;
    do { count = read(fd, buf, sizeof(buf)); } while (count < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (count <= 0) { errno = count < 0 ? saved : ENODATA; return -1; }
    if (count == static_cast<ssize_t>(sizeof(buf))) { errno = EOVERFLOW; return -1; }
    if (memchr(buf, '\0', count)) { errno = EINVAL; return -1; }
    out.assign(buf, count);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) out.pop_back();
    if (out.empty()) { errno = ENODATA; return -1; }
    return 0;
}

int number(int directory, const char *attribute, int32_t &value)
{
    std::string raw;
    if (text(directory, attribute, raw)) return -1;
    if (raw[0] != '-' && (raw[0] < '0' || raw[0] > '9')) { errno = EINVAL; return -1; }
    char *end = nullptr;
    errno = 0;
    long long parsed = strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end || errno || parsed < INT32_MIN || parsed > INT32_MAX) { errno = ERANGE; return -1; }
    value = static_cast<int32_t>(parsed);
    return 0;
}

bool label(std::string &value)
{
    if (value.empty() || value.size() > 100) return false;
    for (unsigned char ch : value) if (ch < 32 || ch == 127 || ch == '/') return false;
    return true;
}

void add(std::vector<AuroraTemperature> &out, const std::string &source, int fd,
         const char *attribute, bool &invalid)
{
    int32_t temperature;
    if (number(fd, attribute, temperature) || temperature < -273150 || temperature > 250000) {
        invalid = true;
        return;
    }
    out.push_back({source, temperature});
}
}

int AuroraReadTemperatures(std::vector<AuroraTemperature> &samples)
{
    samples.clear();
    bool invalid = false;
    for (bool hwmon : {false, true}) {
        DIR *root = opendir(hwmon ? AURORA_HWMON_ROOT : AURORA_THERMAL_ROOT);
        if (!root) {
            if (errno != ENOENT) invalid = true;
            continue;
        }
        while (dirent *entry = readdir(root)) {
            if (!numbered(entry->d_name, hwmon ? "hwmon" : "thermal_zone")) continue;
            // Linux class entries are kernel-managed symlinks; follow only
            // this directory boundary, never individual attribute symlinks.
            int fd = openat(dirfd(root), entry->d_name, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
            if (fd < 0) { invalid = true; continue; }
            std::string identity;
            if (text(fd, hwmon ? "name" : "type", identity) || !label(identity)) {
                invalid = true; close(fd); continue;
            }
            std::string prefix = std::string(hwmon ? "hwmon/" : "thermal/") + entry->d_name + '/' + identity;
            if (!hwmon) {
                add(samples, prefix, fd, "temp", invalid);
                close(fd);
            } else {
                DIR *attributes = fdopendir(fd);
                if (!attributes) { close(fd); invalid = true; continue; }
                while (dirent *attribute = readdir(attributes)) {
                    if (!numbered(attribute->d_name, "temp", "_input")) continue;
                    std::string stem(attribute->d_name, strlen(attribute->d_name) - strlen("_input"));
                    int32_t fault = 0;
                    if (!number(fd, (stem + "_fault").c_str(), fault)) {
                        if (fault) { invalid = true; continue; }
                    } else if (errno != ENOENT) { invalid = true; continue; }
                    std::string sensor;
                    if (!text(fd, (stem + "_label").c_str(), sensor)) {
                        if (!label(sensor)) { invalid = true; continue; }
                    } else if (errno != ENOENT) { invalid = true; continue; }
                    std::string source = prefix + '/' + stem;
                    if (!sensor.empty()) source += ':' + sensor;
                    add(samples, source, fd, attribute->d_name, invalid);
                }
                closedir(attributes);
            }
            if (samples.size() > 512) {
                closedir(root); samples.clear(); errno = EOVERFLOW; return -1;
            }
        }
        closedir(root);
    }
    if (samples.empty()) { errno = invalid ? EIO : ENODEV; return -1; }
    std::sort(samples.begin(), samples.end(), [](const auto &a, const auto &b) { return a.source < b.source; });
    return 0;
}
