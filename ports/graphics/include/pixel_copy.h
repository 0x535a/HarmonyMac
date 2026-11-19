/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <cstdint>
#include <cstring>

// A row without scaling has no per-pixel divisions. memcpy keeps this valid
// for every public buffer stride/alignment; the compiler can vectorize it.
inline void AuroraCopyRow(uint8_t *destination, const uint8_t *source, int width,
                          bool swap_red_blue, bool opaque)
{
    if (!swap_red_blue && !opaque) {
        std::memcpy(destination,source,size_t(width)*4);
        return;
    }
    for (int x=0;x<width;++x) {
        uint32_t pixel;
        std::memcpy(&pixel,source+size_t(x)*4,4);
        if (swap_red_blue) pixel=(pixel&0xff00ff00U)|((pixel&0xffU)<<16)|((pixel>>16)&0xffU);
        if (opaque) pixel|=0xff000000U;
        std::memcpy(destination+size_t(x)*4,&pixel,4);
    }
}
