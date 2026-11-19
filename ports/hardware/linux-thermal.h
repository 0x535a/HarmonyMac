/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AuroraTemperature {
    std::string source;
    int32_t milli_celsius;
};

// Reads Linux thermal and hwmon temperature attributes only. No fallback
// temperatures, fan controls, thresholds or power-policy writes.
int AuroraReadTemperatures(std::vector<AuroraTemperature> &samples);
