/* Test-only calls into the ISO's original HDF composer service. The factory
 * returns the concrete object's base address (verified from this binary).
 * No replacement HDF service or copied private object layout is used.
 */
#include "composer_abi.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
using namespace AuroraComposer;
static void check(bool ok, const char *what)
{ if (!ok) { fprintf(stderr, "COMPOSER_SERVICE_PROBE_FAIL %s\n", what); exit(2); } }
template<typename T> static T symbol(void *library, const char *name)
{ auto value = reinterpret_cast<T>(dlsym(library, name)); check(value, name); return value; }
int main()
{
    setbuf(stdout, nullptr);
    // Preload our SONAME so the unchanged service's dlopen selects this VDI.
    void *vdi = dlopen("/vendor/lib64/aurora-graphics/libdisplay_composer_vdi_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    check(vdi, "Aurora VDI");
    void *library = dlopen("/vendor/lib64/libdisplay_composer_service_1.3.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { puts(dlerror()); return 2; }
    void *object = symbol<void *(*)()>(library, "DisplayComposerImplGetInstance")();
    check(object, "original service factory");
    auto load_so = symbol<int (*)(void *)>(library,
        "_ZN4OHOS3HDI7Display8Composer22DisplayComposerService9LoadVdiSoEv");
    check(!load_so(object), "original service VDI dlopen");
    auto load = symbol<int (*)(void *)>(library,
        "_ZN4OHOS3HDI7Display8Composer22DisplayComposerService14LoadVdiAdapterEv");
    check(!load(object), "all mandatory VDI symbols accepted by original adapter");
    auto capability = symbol<int (*)(void *, uint32_t, Capability *)>(library,
        "_ZN4OHOS3HDI7Display8Composer22DisplayComposerService20GetDisplayCapabilityEjRNS2_4V1_017DisplayCapabilityE");
    auto modes = symbol<int (*)(void *, uint32_t, std::vector<Mode> *)>(library,
        "_ZN4OHOS3HDI7Display8Composer22DisplayComposerService24GetDisplaySupportedModesEjRNSt3__h6vectorINS2_4V1_015DisplayModeInfoENS4_9allocatorIS7_EEEE");
    Capability cap;
    std::vector<Mode> list;
    check(!capability(object, 0, &cap) && cap.name == "Aurora internal display" && cap.supportLayers == 0,
          "capability forwarded through original service");
    check(!modes(object, 0, &list) && list.size() == 1 && list[0].width > 0 && list[0].height > 0,
          "mode vector forwarded through original service");
    symbol<void (*)(void *)>(library,
        "_ZN4OHOS3HDI7Display8Composer22DisplayComposerServiceD0Ev")(object);
    dlclose(library); dlclose(vdi);
    printf("ORIGINAL_COMPOSER_SERVICE_PASS mode=%dx%d@%u platform-string/vector/loader\n",
           list[0].width, list[0].height, list[0].freshRate);
    return 0;
}
