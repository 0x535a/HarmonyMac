/* SPDX-License-Identifier: Apache-2.0 */
#include "linux-iio.h"
#include <cerrno>
#include <iomanip>
#include <iostream>
int main(int argc, char **)
{
    if (argc != 1) { std::cerr << "usage: iio-sensors\n"; return 2; }
    std::vector<AuroraIioChannel> channels;
    if (AuroraDiscoverIio(channels)) {
        int error = errno;
        std::cout << "IIO_DISCOVERY_ERROR errno=" << error << '\n';
        return 1;
    }
    unsigned errors = 0;
    for (const auto &channel : channels) {
        AuroraIioSample sample{};
        int result = AuroraReadIio(channel, sample), error = errno;
        bool light = channel.kind == AuroraIioKind::Light;
        std::cout << "IIO kind=" << (light ? "light" : "lid-angle") << " name=" << std::quoted(channel.name)
            << " source=" << std::quoted(channel.device + "/" + channel.attribute);
        if (result) { ++errors; std::cout << " error=" << error; }
        else std::cout << std::setprecision(15) << " value=" << sample.value << " unit=" << (light ? "lux" : "degrees")
            << " timestamp_boottime_ns=" << sample.timestamp_ns;
        std::cout << '\n';
    }
    return errors ? 2 : 0;
}
