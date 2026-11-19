/* SPDX-License-Identifier: Apache-2.0 */
#include "linux-thermal.h"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
static void write(const fs::path &path, const char *text)
{ fs::create_directories(path.parent_path()); std::ofstream(path) << text; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    fs::path root(argv[1]);
    std::vector<AuroraTemperature> samples;
    assert(AuroraReadTemperatures(samples) == -1 && errno == ENODEV && samples.empty());
    auto h = root / "hwmon/hwmon77";
    auto t = root / "thermal/thermal_zone22";
    write(h / "name", "apple_smc\n");
    write(h / "temp1_input", "41000\n");
    write(h / "temp1_label", "CPU core\n");
    write(h / "temp2_input", "123000\n");
    write(h / "temp2_fault", "1\n");
    write(t / "type", "macsmc-soc\n");
    write(t / "temp", "42000\n");
    assert(!AuroraReadTemperatures(samples) && samples.size() == 2);
    assert(samples[0].milli_celsius == 41000 && samples[1].milli_celsius == 42000);
    assert(samples[0].source == "hwmon/hwmon77/apple_smc/temp1:CPU core");
    write(h / "temp1_input", "-1000\n");
    assert(!AuroraReadTemperatures(samples) && samples[0].milli_celsius == -1000);
    write(h / "temp1_input", "99999999999999999999\n");
    write(t / "temp", "oops\n");
    assert(AuroraReadTemperatures(samples) == -1 && errno == EIO && samples.empty());
    write(t / "temp", "43000\n");
    assert(!AuroraReadTemperatures(samples) && samples.size() == 1 && samples[0].milli_celsius == 43000);
    write(root / "external", "12000\n");
    fs::remove(t / "temp");
    fs::create_symlink(root / "external", t / "temp");
    assert(AuroraReadTemperatures(samples) == -1 && errno == EIO && samples.empty());
    fs::remove_all(root / "thermal");
    fs::remove_all(root / "hwmon");
    assert(AuroraReadTemperatures(samples) == -1 && errno == ENODEV && samples.empty());
    std::cout << "THERMAL_READER_REAL_DISCOVERY_UNITS_FAULT_INVALID_REMOVAL_PASS\n";
}
