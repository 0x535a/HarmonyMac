/* SPDX-License-Identifier: Apache-2.0
 * Data-only translation of the pinned upstream DisplayBufferType.idl beside
 * this file. No HDF RPC implementation is generated here.
 */
#ifndef AURORA_DISPLAY_BUFFER_TYPE_V10_H
#define AURORA_DISPLAY_BUFFER_TYPE_V10_H
#include <cstdint>
namespace OHOS::HDI::Display::Buffer::V1_0 {
struct AllocInfo {
    uint32_t width;
    uint32_t height;
    uint64_t usage;
    uint32_t format;
    uint32_t expectedSize;
};
struct VerifyAllocInfo {
    uint32_t width;
    uint32_t height;
    uint64_t usage;
    uint32_t format;
};
static_assert(sizeof(AllocInfo) == 24);
static_assert(sizeof(VerifyAllocInfo) == 24);
}
#endif
