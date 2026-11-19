/* SPDX-License-Identifier: GPL-3.0-only
 * Calls exported methods on an object constructed by the original DSO. Does
 * not guess object layouts or replace callbacks/identity. Synthetic sysfs only.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *symbol(void *lib, const char *name)
{
    void *result = dlsym(lib, name);
    if (!result) { fprintf(stderr, "BATTERY_ORIGINAL_SYMBOL_FAIL %s %s\n", name, dlerror()); exit(2); }
    return result;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL); alarm(15);
    void *lib = dlopen("/vendor/lib64/libbattery_interface_service_2.0.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    void *(*factory)(void) = (void *(*)(void))symbol(lib, "BatteryInterfaceImplGetInstance");
    void *object = factory();
    if (!object) { puts("BATTERY_ORIGINAL_FACTORY_FAIL"); return 2; }
    if (argc == 2 && !strcmp(argv[1], "--missing")) {
        int (*get)(void *, int32_t *) = (int (*)(void *, int32_t *))symbol(lib,
            "_ZN4OHOS3HDI7Battery4V2_020BatteryInterfaceImpl11GetCapacityERi");
        int32_t value = -9999;
        int result = get(object, &value);
        printf("BATTERY_ORIGINAL_ABSENT status=%d value=%d\n", result, value);
        if (!result || value != -9999) return 2;
        puts("BATTERY_ORIGINAL_ABSENT_PASS synthetic_sysfs=1");
        return 0;
    }
    struct { const char *name; int expected; } tests[] = {
        {"_ZN4OHOS3HDI7Battery4V2_020BatteryInterfaceImpl11GetCapacityERi", 68},
        {"_ZN4OHOS3HDI7Battery4V2_020BatteryInterfaceImpl10GetVoltageERi", 11800000},
        {"_ZN4OHOS3HDI7Battery4V2_020BatteryInterfaceImpl14GetTemperatureERi", 307},
        {"_ZN4OHOS3HDI7Battery4V2_020BatteryInterfaceImpl13GetCurrentNowERi", -1200000},
    };
    for (unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i) {
        int (*get)(void *, int32_t *) = (int (*)(void *, int32_t *))symbol(lib, tests[i].name);
        int32_t value = -9999;
        int status = get(object, &value);
        printf("BATTERY_ORIGINAL_READ index=%u status=%d value=%d expected=%d\n", i, status, value, tests[i].expected);
        if (status || value != tests[i].expected) return 2;
    }
    puts("BATTERY_ORIGINAL_PROVIDER_PASS synthetic_sysfs=1 real_hardware=0 hdf_ipc=0");
    // Original Init creates a worker. Keep its DSO and object alive until exit.
    return 0;
}
