/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "buffer_handle.h"
#include "display_type.h"

inline bool AuroraIsLinearYuv420(uint32_t format)
{
    return format == PIXEL_FMT_YCBCR_420_SP || format == PIXEL_FMT_YCRCB_420_SP;
}

inline bool AuroraValidLinearYuv420(const BufferHandle &buffer)
{
    return AuroraIsLinearYuv420(buffer.format) && buffer.fd >= 0 &&
        buffer.width > 0 && buffer.width <= 16384 && buffer.width % 2 == 0 &&
        buffer.height > 0 && buffer.height <= 16384 && buffer.height % 2 == 0 &&
        buffer.stride >= buffer.width && buffer.stride % 2 == 0 &&
        int64_t(buffer.stride) * (buffer.height + buffer.height / 2) <= buffer.size &&
        !(buffer.usage & HBM_USE_PROTECTED);
}
