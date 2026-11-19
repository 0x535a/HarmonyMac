/* SPDX-License-Identifier: Apache-2.0
 * Read-only IIO sysfs consumer. No firmware/calibration/sample-rate writes.
 */
#include "linux-iio.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
int text(int dir, const std::string &name, std::string &value)
{
    Fd fd(openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0) return -1;
    char buffer[256];
    ssize_t count;
    do { count = read(fd.value, buffer, sizeof(buffer)); } while (count < 0 && errno == EINTR);
    if (count < 0) return -1;
    if (!count || count == static_cast<ssize_t>(sizeof(buffer))) { errno = EINVAL; return -1; }
    value.assign(buffer, static_cast<size_t>(count));
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (value.empty() || value.find('\0') != std::string::npos) { errno = EINVAL; return -1; }
    return 0;
}
int number(int dir, const std::string &name, double &value)
{
    std::string input;
    if (text(dir, name, input)) return -1;
    // Kernel numeric attributes are single decimal numbers, never locale text.
    if (input.find_first_not_of("0123456789+-.eE") != std::string::npos) { errno = EINVAL; return -1; }
    char *end = nullptr;
    errno = 0;
    double parsed = strtod(input.c_str(), &end);
    if (errno || !end || end == input.c_str() || *end || !std::isfinite(parsed)) {
        if (!errno) errno = EINVAL;
        return -1;
    }
    value = parsed;
    return 0;
}
bool exists(int dir, const char *attribute)
{
    struct stat st;
    return !fstatat(dir, attribute, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);
}
bool deviceName(const char *name)
{
    const std::string prefix = "iio:device";
    std::string s(name);
    return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0 &&
        s.find_first_not_of("0123456789", prefix.size()) == std::string::npos;
}
}

int AuroraDiscoverIio(std::vector<AuroraIioChannel> &channels, const std::string &root)
{
    channels.clear();
    DIR *directory = opendir(root.c_str());
    if (!directory) { if (errno == ENOENT) errno = ENODEV; return -1; }
    std::vector<AuroraIioChannel> found;
    int error = 0;
    unsigned count = 0;
    while (auto *entry = readdir(directory)) {
        if (!deviceName(entry->d_name)) continue;
        if (++count > 256) { error = E2BIG; break; }
        std::string path = root + "/" + entry->d_name;
        Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
        struct stat st;
        std::string name;
        if (fd.value < 0 || fstat(fd.value, &st) || text(fd.value, "name", name)) continue;
        AuroraIioChannel channel{AuroraIioKind::Light, path, name, "", "", "", static_cast<uint64_t>(st.st_ino)};
        if (exists(fd.value, "in_illuminance_input")) {
            channel.attribute = "in_illuminance_input";
            found.push_back(channel);
        } else if (exists(fd.value, "in_illuminance_raw") && exists(fd.value, "in_illuminance_scale")) {
            channel.attribute = "in_illuminance_raw";
            channel.scale = "in_illuminance_scale";
            if (exists(fd.value, "in_illuminance_offset")) channel.offset = "in_illuminance_offset";
            found.push_back(channel);
        }
        // AOP LAS intentionally exports integral degrees as RAW without scale.
        // Do not assume that unit for other IIO angular sensors.
        if (name == "aop-sensors-las" && exists(fd.value, "in_angl_raw")) {
            channel.kind = AuroraIioKind::LidAngle;
            channel.attribute = "in_angl_raw";
            channel.scale.clear(); channel.offset.clear(); channel.aop_degrees = true;
            found.push_back(channel);
        }
    }
    closedir(directory);
    if (error) { errno = error; return -1; }
    if (found.empty()) { errno = ENODEV; return -1; }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.device != b.device) return a.device < b.device;
        return a.attribute < b.attribute;
    });
    channels = std::move(found);
    return 0;
}

int AuroraReadIio(const AuroraIioChannel &channel, AuroraIioSample &sample)
{
    if (channel.kind == AuroraIioKind::LidAngle && !channel.aop_degrees) { errno = ENOTSUP; return -1; }
    Fd fd(open(channel.device.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (fd.value < 0) return -1;
    struct stat st;
    std::string name;
    if (fstat(fd.value, &st) || text(fd.value, "name", name)) return -1;
    if (static_cast<uint64_t>(st.st_ino) != channel.inode || name != channel.name) { errno = ESTALE; return -1; }
    double value, scale = 1, offset = 0;
    if (number(fd.value, channel.attribute, value)) return -1;
    if (!channel.scale.empty() && number(fd.value, channel.scale, scale)) return -1;
    if (!channel.offset.empty() && number(fd.value, channel.offset, offset)) return -1;
    if (scale <= 0) { errno = ERANGE; return -1; }
    value = (value + offset) * scale;
    if (!std::isfinite(value) || value < 0 ||
        (channel.kind == AuroraIioKind::Light && value > 2147483647.999999) ||
        (channel.kind == AuroraIioKind::LidAngle && (value > 255 || std::floor(value) != value))) {
        errno = ERANGE; return -1;
    }
    timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now)) return -1;
    sample = {value, static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec};
    return 0;
}
