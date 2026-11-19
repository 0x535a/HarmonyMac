/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AURORA_LINUX_IIO_H
#define AURORA_LINUX_IIO_H
#include <cstdint>
#include <string>
#include <vector>

enum class AuroraIioKind { Light, LidAngle };
struct AuroraIioChannel {
    AuroraIioKind kind;
    std::string device, name, attribute, scale, offset;
    uint64_t inode;
    // The AOP LAS raw attribute already contains degrees, unlike generic IIO.
    bool aop_degrees = false;
};
struct AuroraIioSample {
    double value;
    int64_t timestamp_ns;
};
// Returns -1 with errno for absent/invalid/unreadable devices, never a fake zero.
int AuroraDiscoverIio(std::vector<AuroraIioChannel> &channels,
    const std::string &root = "/sys/bus/iio/devices");
int AuroraReadIio(const AuroraIioChannel &channel, AuroraIioSample &sample);
#endif
