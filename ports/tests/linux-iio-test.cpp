/* SPDX-License-Identifier: Apache-2.0
 * Unit/identity/error tests; temporary files do not qualify kernel or HDI I/O.
 */
#include "linux-iio.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
static void put(const fs::path &p, const std::string &s) { std::ofstream f(p); f << s; f.close(); assert(f); }
static size_t fds()
{
    DIR *d = opendir("/proc/self/fd"); assert(d); size_t n = 0;
    while (readdir(d)) ++n;
    closedir(d); return n;
}
int main()
{
    char directory[] = "/tmp/aurora-iio-unit-XXXXXX";
    assert(mkdtemp(directory)); fs::path root(directory);
    std::vector<AuroraIioChannel> channels;
    assert(AuroraDiscoverIio(channels, root) == -1 && errno == ENODEV && channels.empty());
    auto als = root / "iio:device0", raw = root / "iio:device1", las = root / "iio:device2", unknown = root / "iio:device3";
    for (const auto &d : {als, raw, las, unknown}) fs::create_directory(d);
    put(als / "name", "aop-sensors-als\n"); put(als / "in_illuminance_input", "123.456789\n");
    put(raw / "name", "scaled-als\n"); put(raw / "in_illuminance_raw", "22\n");
    put(raw / "in_illuminance_scale", "0.125\n"); put(raw / "in_illuminance_offset", "2\n");
    put(las / "name", "aop-sensors-las\n"); put(las / "in_angl_raw", "245\n");
    put(unknown / "name", "unknown-angle\n"); put(unknown / "in_angl_raw", "42\n");
    assert(AuroraDiscoverIio(channels, root) == 0 && channels.size() == 3);
    auto processed = channels[0], angle = channels[1], scaled = channels[2];
    AuroraIioSample sample{};
    assert(AuroraReadIio(processed, sample) == 0 && std::abs(sample.value - 123.456789) < 1e-9 && sample.timestamp_ns > 0);
    assert(AuroraReadIio(scaled, sample) == 0 && sample.value == 3);
    assert(AuroraReadIio(angle, sample) == 0 && sample.value == 245);
    for (const std::string &bad : std::vector<std::string>{"", "-1", "nan", "inf", "1e309", "1e100", "12junk", " 12", "12 13", std::string(300, '1')}) {
        put(als / "in_illuminance_input", bad); sample.value = 987;
        assert(AuroraReadIio(processed, sample) == -1 && sample.value == 987);
    }
    put(als / "in_illuminance_input", "0\n");
    assert(AuroraReadIio(processed, sample) == 0 && sample.value == 0);
    put(raw / "in_illuminance_scale", "0\n"); assert(AuroraReadIio(scaled, sample) == -1 && errno == ERANGE);
    put(raw / "in_illuminance_scale", "0.125\n");
    for (const char *bad : {"256", "42.5", "-1"}) { put(las / "in_angl_raw", bad); assert(AuroraReadIio(angle, sample) == -1); }
    put(las / "in_angl_raw", "255\n"); assert(AuroraReadIio(angle, sample) == 0 && sample.value == 255);
    size_t before = fds(); for (int i = 0; i < 1024; ++i) assert(AuroraReadIio(processed, sample) == 0); assert(fds() == before);
    put(als / "name", "different-device\n"); assert(AuroraReadIio(processed, sample) == -1 && errno == ESTALE);
    fs::rename(als, root / "old-identity"); fs::create_directory(als);
    put(als / "name", "aop-sensors-als\n"); put(als / "in_illuminance_input", "99\n");
    assert(AuroraReadIio(processed, sample) == -1 && errno == ESTALE);
    fs::remove_all(root);
    assert(AuroraReadIio(scaled, sample) == -1 && errno == ENOENT);
    std::cout << "IIO_UNIT_UNITS_ERRORS_IDENTITY_AND_FD_LIFECYCLE_PASS\n";
}
