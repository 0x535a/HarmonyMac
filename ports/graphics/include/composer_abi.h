/* SPDX-License-Identifier: GPL-3.0-only
 * Data layouts from drivers_interface DisplayComposerType.idl at
 * 9eaf9abff9c390f6a4bf95271cf482e72c3b8c29. C++ containers must use the
 * target platform's std::__h ABI, not the public SDK's std::__n1.
 */
#ifndef AURORA_COMPOSER_ABI_H
#define AURORA_COMPOSER_ABI_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace AuroraComposer {
struct Property { std::string name; uint32_t propId; uint64_t value; };
struct Capability {
    std::string name;
    int32_t type;
    uint32_t phyWidth, phyHeight, supportLayers, virtualDispCount;
    bool supportWriteBack;
    uint32_t propertyCount;
    std::vector<Property> props;
};
struct Mode { int32_t width, height; uint32_t freshRate; int32_t id; };
struct Rect { int32_t x, y, w, h; };
struct LayerInfo { int32_t width, height, type, bpp, pixFormat; };
struct Alpha { bool enGlobalAlpha, enPixelAlpha; uint8_t alpha0, alpha1, gAlpha; };
struct Color { uint8_t r, g, b, a; };
using Hotplug = void (*)(uint32_t, bool, void *);
using Vblank = void (*)(unsigned int, uint64_t, void *);
static_assert(sizeof(Property) == 40 && sizeof(Capability) == 80);
static_assert(offsetof(Capability, props) == 56);
static_assert(sizeof(Mode) == 16 && sizeof(LayerInfo) == 20 && sizeof(Alpha) == 5);
}
#endif
