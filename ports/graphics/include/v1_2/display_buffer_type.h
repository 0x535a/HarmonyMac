/* SPDX-License-Identifier: Apache-2.0
 * Data-only translation of the pinned upstream DisplayBufferType.idl.
 */
#ifndef AURORA_DISPLAY_BUFFER_TYPE_V12_H
#define AURORA_DISPLAY_BUFFER_TYPE_V12_H
#include <vector>
#include "v1_0/display_buffer_type.h"
namespace OHOS::HDI::Display::Buffer::V1_2 {
using V1_0::AllocInfo;
using V1_0::VerifyAllocInfo;
struct ImagePlane {
    uint32_t offset;
    uint32_t hStride;
    uint32_t vStride;
};
struct ImageLayout {
    uint32_t pixelFormat;
    std::vector<ImagePlane> planes;
};
static_assert(sizeof(ImageLayout) == 32);
}
#endif
