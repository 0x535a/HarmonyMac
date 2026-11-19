/* SPDX-License-Identifier: GPL-3.0-only
 * Official generated C client -> original Battery HDF service, over Binder.
 * Default mode only reads the absent battery in a virtual machine. Synthetic
 * positive paths are opt-in, test-labelled, and must be restored afterwards.
 */
#include "battery/v2_0/ibattery_interface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_uint callbacks;
static atomic_int last_capacity;
static int32_t on_update(struct IBatteryCallback *self, const struct BatteryInfo *info)
{
    (void)self;
    if (!info) return -1;
    printf("BATTERY_IPC_CALLBACK capacity=%d voltage=%d synthetic=1\n", info->capacity, info->voltage);
    atomic_fetch_add(&callbacks, 1);
    atomic_store(&last_capacity, info->capacity);
    return 0;
}
static int32_t callback_version(struct IBatteryCallback *self, uint32_t *major, uint32_t *minor)
{
    (void)self;
    if (!major || !minor) return -1;
    *major = 2; *minor = 0; return 0;
}
static struct IBatteryCallback callback = {.Update=on_update, .GetVersion=callback_version};

int main(int argc, char **argv)
{
    setbuf(stdout, NULL); alarm(25);
    struct IBatteryInterface *battery = IBatteryInterfaceGetInstance("battery_interface_service", false);
    if (!battery) { puts("BATTERY_IPC_GET_FAILED"); return 2; }
    uint32_t major = 0, minor = 0;
    int result = battery->GetVersion(battery, &major, &minor);
    printf("BATTERY_IPC_VERSION result=%d major=%u minor=%u\n", result, major, minor);
    assert(result == 0 && major == 2 && minor == 0);
    int32_t capacity = -9999;
    result = battery->GetCapacity(battery, &capacity);
    printf("BATTERY_IPC_READ result=%d capacity=%d client_pid=%d uid=%d\n", result, capacity, getpid(), getuid());
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--absent"))) {
        assert(result != 0 && capacity == -9999);
        puts("BATTERY_IPC_ABSENT_PASS hdf_ipc=1 synthetic=0");
    } else if (argc == 2 && !strcmp(argv[1], "--kernel-test")) {
        assert(access("/sys/module/test_power/parameters/battery_capacity", F_OK) == 0);
        assert(battery->ChangePath(battery, "/sys/class/power_supply") == 0);
        FILE *parameter = fopen("/sys/module/test_power/parameters/battery_capacity", "w");
        assert(parameter && fputs("68\n", parameter) >= 0 && fclose(parameter) == 0);
        assert(battery->GetCapacity(battery, &capacity) == 0 && capacity == 68);
        assert(battery->Register(battery, &callback) == 0);
        parameter = fopen("/sys/module/test_power/parameters/battery_capacity", "w");
        assert(parameter && fputs("68\n", parameter) >= 0 && fclose(parameter) == 0);
        for (int i=0; i<100 && atomic_load(&last_capacity)!=68; ++i) usleep(50000);
        int first_capacity = atomic_load(&last_capacity);
        unsigned before_change = atomic_load(&callbacks);
        parameter = fopen("/sys/module/test_power/parameters/battery_capacity", "w");
        assert(parameter && fputs("69\n", parameter) >= 0 && fclose(parameter) == 0);
        for (int i=0; i<100 && atomic_load(&last_capacity)!=69; ++i) usleep(50000);
        unsigned delivered = atomic_load(&callbacks);
        int observed = atomic_load(&last_capacity);
        int actual = -1;
        int read_result = battery->GetCapacity(battery, &actual);
        printf("BATTERY_KERNEL_EVENT_RESULT before=%u first=%d callbacks=%u observed=%d actual=%d read_result=%d test_driver=1\n",
               before_change, first_capacity, delivered, observed, actual, read_result);
        int unregistered = battery->UnRegister(battery);
        parameter = fopen("/sys/module/test_power/parameters/battery_capacity", "w");
        assert(parameter && fputs("68\n", parameter) >= 0 && fclose(parameter) == 0);
        IBatteryInterfaceReleaseInstance("battery_interface_service", battery, false);
        if (!before_change || first_capacity != 68 || delivered <= before_change || observed != 69 ||
            read_result || actual != 69 || unregistered) return 2;
        puts("BATTERY_KERNEL_CALLBACK_PASS real_kernel_uevent=1 synthetic_driver=1 physical_battery=0");
        return 0;
    } else if (argc == 2 && !strcmp(argv[1], "--synthetic")) {
        // This labelled data tree is created only in the disposable virtual
        // candidate; never write to sysfs or manufacture kernel uevents.
        const char *path = "/data/boot/aurora/tests/power_supply";
        assert(access("/data/boot/aurora/tests/power_supply/.synthetic-only", F_OK) == 0);
        assert(battery->ChangePath(battery, path) == 0);
        int32_t voltage = -1, temperature = -1, current = 0;
        int a = battery->GetCapacity(battery, &capacity);
        int b = battery->GetVoltage(battery, &voltage);
        int c = battery->GetTemperature(battery, &temperature);
        int d = battery->GetCurrentNow(battery, &current);
        printf("BATTERY_IPC_SYNTHETIC_READ results=%d,%d,%d,%d capacity=%d voltage=%d temp=%d current=%d\n",
               a,b,c,d,capacity,voltage,temperature,current);
        int good = !a && !b && !c && !d && capacity==68 && voltage==11800000 && temperature==307 && current==-1200000;
        int registered = battery->Register(battery, &callback);
        printf("BATTERY_IPC_REGISTER result=%d\n", registered);
        if (!registered) {
            sleep(2);
            int unregistered = battery->UnRegister(battery);
            printf("BATTERY_IPC_UNREGISTER result=%d observed_callbacks=%u\n", unregistered, atomic_load(&callbacks));
            good = good && unregistered == 0;
        }
        int restored = battery->ChangePath(battery, "/sys/class/power_supply");
        printf("BATTERY_IPC_PATH_RESTORED result=%d\n", restored);
        IBatteryInterfaceReleaseInstance("battery_interface_service", battery, false);
        assert(good && restored == 0 && registered == 0);
        puts("BATTERY_IPC_SYNTHETIC_PASS hdf_ipc=1 synthetic=1 physical_battery=0");
        return 0;
    } else {
        fprintf(stderr, "unsupported mode\n");
        IBatteryInterfaceReleaseInstance("battery_interface_service", battery, false);
        return 2;
    }
    IBatteryInterfaceReleaseInstance("battery_interface_service", battery, false);
    return 0;
}
