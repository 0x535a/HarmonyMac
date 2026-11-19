/* SPDX-License-Identifier: GPL-3.0-only
 * Shared frame-content fingerprints for the RenderService output and the
 * Composer scanout. Both sides sample the same grid and normalize to RGB, so a
 * frame can be followed from the composited source to the scanned buffer.
 *
 * A 32-cell serial marker is stamped into the source frame's top four rows at
 * the panel centre, which the camera housing covers when the notch rows are
 * enabled. The grid never samples those rows.
 */
#pragma once
#include "display_type.h"
#include <cstdint>
#include <cstdio>

namespace AuroraFrameSamples {
constexpr int marker_cells = 32, marker_cell = 4, marker_rows = 4, skip_rows = 8;
constexpr int grid_columns = 64, grid_rows = 40, region_columns = 8, region_rows = 8;
constexpr int regions = region_columns * region_rows;
constexpr uint32_t serial_mask = 0xffffff;

inline int marker_left(int width) { return width / 2 - marker_cells * marker_cell / 2; }
inline bool marker_fits(int width, int height)
{
    return width >= 2 * marker_cells * marker_cell && height > skip_rows + grid_rows;
}
inline unsigned checksum(uint32_t serial)
{
    unsigned sum = 0;
    for (int shift = 0; shift < 24; shift += 4) sum += (serial >> shift) & 0xf;
    return sum & 0xf;
}
// Cell values, left to right: 1 0, serial bits 0..23, checksum bits 0..3, 0 1.
inline bool marker_bit(uint32_t serial, int cell)
{
    if (cell == 0 || cell == marker_cells - 1) return true;
    if (cell == 1 || cell == marker_cells - 2) return false;
    if (cell < 26) return (serial >> (cell - 2)) & 1;
    return (checksum(serial) >> (cell - 26)) & 1;
}
inline bool rgba_order(int format)
{
    return format == PIXEL_FMT_RGBA_8888 || format == PIXEL_FMT_RGBX_8888;
}
inline void rgb(const uint8_t *pixel, bool rgba, uint8_t out[3])
{
    out[0] = pixel[rgba ? 0 : 2];
    out[1] = pixel[1];
    out[2] = pixel[rgba ? 2 : 0];
}

struct Result {
    int serial = -1;  // decoded marker, or -1 when absent or covered
    uint16_t region[regions] = {};
};

// Pixels are top-left origin rows of a mapped linear 32-bit buffer.
inline Result sample(const uint8_t *pixels, int width, int height, int stride, int format)
{
    Result result;
    const bool rgba = rgba_order(format);
    uint32_t hash[regions];
    for (auto &value : hash) value = 2166136261u;
    const int usable = height - skip_rows;
    for (int gy = 0; gy < grid_rows; ++gy) {
        const int y = skip_rows + (2 * gy + 1) * usable / (2 * grid_rows);
        const uint8_t *row = pixels + static_cast<size_t>(y) * stride;
        const int region_row = gy * region_rows / grid_rows;
        for (int gx = 0; gx < grid_columns; ++gx) {
            const int x = (2 * gx + 1) * width / (2 * grid_columns);
            uint8_t value[3];
            rgb(row + static_cast<size_t>(x) * 4, rgba, value);
            auto &h = hash[region_row * region_columns + gx * region_columns / grid_columns];
            for (auto byte : value) h = (h ^ byte) * 16777619u;
        }
    }
    for (int i = 0; i < regions; ++i) result.region[i] = static_cast<uint16_t>(hash[i] ^ (hash[i] >> 16));
    if (!marker_fits(width, height)) return result;
    const uint8_t *row = pixels + static_cast<size_t>(marker_rows / 2) * stride;
    uint32_t serial = 0;
    for (int cell = 0; cell < marker_cells; ++cell) {
        const int x = marker_left(width) + cell * marker_cell + marker_cell / 2;
        uint8_t value[3];
        rgb(row + static_cast<size_t>(x) * 4, rgba, value);
        const bool bit = unsigned(value[0]) + value[1] + value[2] > 384;
        if (cell >= 2 && cell < 26 && bit) serial |= 1u << (cell - 2);
        if ((cell < 2 || cell >= 26) && bit != marker_bit(serial, cell)) return result;
    }
    result.serial = static_cast<int>(serial);
    return result;
}

// 64 regions x 4 hex digits.
inline void format_regions(const Result &result, char out[regions * 4 + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < regions; ++i)
        for (int nibble = 0; nibble < 4; ++nibble)
            out[i * 4 + nibble] = digits[(result.region[i] >> (12 - 4 * nibble)) & 0xf];
    out[regions * 4] = 0;
}
} // namespace AuroraFrameSamples
