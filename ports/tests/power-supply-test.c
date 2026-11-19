/* SPDX-License-Identifier: GPL-3.0-only
 * Synthetic class files and real fd/ioctl lifecycle, not physical battery proof.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#include <limits.h>
static char fixture_root[PATH_MAX];
#define AURORA_POWER_SUPPLY_ROOT fixture_root
#include "../hardware/power_supply.c"
#define L410_BATTERY_ROOT AuroraBatteryRoot()
#include "../hardware/xpower_battery_v303.c"

static void put(const char *base, const char *name, const char *value)
{
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/%s", base, name) < (int)sizeof(path));
    FILE *f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0 && fclose(f) == 0);
}

int main(void)
{
    char root[] = "/tmp/aurora-battery-XXXXXX";
    char *base = mkdtemp(root);
    assert(base);
    snprintf(fixture_root, sizeof(fixture_root), "%s/power_supply", base);
    assert(mkdir(fixture_root, 0700) == 0);
    char battery[128], charger[128], peripheral[128], second[128], selected[PATH_MAX];
    snprintf(battery, sizeof(battery), "%s/power_supply/macsmc-battery", base);
    snprintf(charger, sizeof(charger), "%s/power_supply/macsmc-ac", base);
    snprintf(peripheral, sizeof(peripheral), "%s/power_supply/hid-battery", base);
    snprintf(second, sizeof(second), "%s/power_supply/another-battery", base);
    assert(AuroraFindBattery(selected, sizeof(selected)) == -1 && errno == ENODEV);
    assert(mkdir(battery, 0700) == 0 && mkdir(charger, 0700) == 0 && mkdir(peripheral, 0700) == 0);
    put(battery, "type", "Battery\n"); put(battery, "scope", "System\n");
    put(charger, "type", "Mains\n");
    put(peripheral, "type", "Battery\n"); put(peripheral, "scope", "Device\n");
    assert(AuroraFindBattery(selected, sizeof(selected)) == 0 && !strcmp(selected, battery));
    assert(AuroraFindBattery(selected, 2) == -1 && errno == ENAMETOOLONG);
    struct { const char *name, *text; int prop, value; } values[] = {
        {"charge_now", "2400000\n", 0, 2400}, {"voltage_now", "11800000\n", 1, 11800},
        {"current_now", "-1200000\n", 2, -1200000}, {"cycle_count", "42\n", 3, 42},
        {"charge_full", "4800000\n", 4, 4800}, {"capacity", "50\n", 5, 50},
        {"status", "Discharging\n", 7, 0}, {"health", "Good\n", 8, 1},
        {"temp", "307\n", 9, 307}, {"charge_full", "4800000\n", 10, 4800000},
        {"charge_now", "2400000\n", 12, 2400000}, {"charge_full_design", "5000000\n", 13, 5000},
    };
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        put(battery, values[i].name, values[i].text);
        union battery_value arg = {.value = values[i].prop};
        assert(l410_xpower_battery_value(&arg) == 0 && arg.value == values[i].value);
    }
    union battery_value arg = {.value = 6};
    assert(l410_xpower_battery_value(&arg) == 0 && !strcmp(arg.text, "Unknown"));
    put(battery, "manufacturer", "Apple\n"); arg.value = 6;
    assert(l410_xpower_battery_value(&arg) == 0 && !strcmp(arg.text, "Apple"));
    arg.value = 11; assert(l410_xpower_battery_value(&arg) == -1 && errno == EOPNOTSUPP);
    put(battery, "capacity", "junk\n"); arg.value = 5;
    assert(l410_xpower_battery_value(&arg) == -1 && errno == EINVAL && arg.value == 5);
    put(battery, "current_now", "2147483648\n"); arg.value = 2;
    assert(l410_xpower_battery_value(&arg) == -1 && errno == ERANGE);
    put(battery, "status", "Charging\n"); arg.value = 7;
    assert(l410_xpower_battery_value(&arg) == 0 && arg.value == 1);
    put(battery, "capacity", "50\n");
    int fd = l410_xpower_open("/dev/dubai", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0); arg.value = 5;
    assert(l410_xpower_ioctl(fd, BATTERY_IOC, &arg) == 0 && arg.value == 50);
    assert(l410_xpower_ioctl(fd, 123UL, &arg) == -1 && errno == ENOTTY);
    assert(l410_xpower_close(fd) == 0);
    int unrelated = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(unrelated >= 0 && l410_xpower_ioctl(unrelated, BATTERY_IOC, &arg) == -1 && errno == ENOTTY);
    close(unrelated);
    assert(mkdir(second, 0700) == 0); put(second, "type", "Battery\n");
    assert(AuroraFindBattery(selected, sizeof(selected)) == -1 && errno == EBUSY);
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/type", second);
    assert(unlink(path) == 0 && rmdir(second) == 0);
    DIR *dir = opendir(battery); assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", battery, entry->d_name); assert(unlink(path) == 0);
    }
    closedir(dir); assert(rmdir(battery) == 0);
    arg.value = 6;
    assert(l410_xpower_battery_value(&arg) == -1 && errno == ENODEV);
    const char *others[] = {charger, peripheral};
    for (unsigned i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/type", others[i]); assert(unlink(path) == 0);
        snprintf(path, sizeof(path), "%s/scope", others[i]); unlink(path);
        assert(rmdir(others[i]) == 0);
    }
    assert(rmdir(fixture_root) == 0 && rmdir(base) == 0);
    puts("AURORA_POWER_SUPPLY_TEST_PASS synthetic_sysfs=1 real_hardware=0");
    return 0;
}
