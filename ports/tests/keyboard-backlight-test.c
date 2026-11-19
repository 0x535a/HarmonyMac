/* SPDX-License-Identifier: GPL-3.0-only
 * Synthetic sysfs tests only; never accesses a physical keyboard light.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#include <limits.h>
static char fixture_root[PATH_MAX];
#define AURORA_KEYBOARD_LIGHT_ROOT fixture_root
#include "../hardware/keyboard-backlight.c"

static void put(const char *base, const char *name, const char *text)
{
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/%s", base, name) < (int)sizeof(path));
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

int main(void)
{
    char temporary[] = "/tmp/aurora-keyboardlight-XXXXXX";
    char *base = mkdtemp(temporary);
    assert(base);
    char hardware[128], entry[128], path[PATH_MAX];
    char indicator[128], indicator_link[128];
    snprintf(fixture_root, sizeof(fixture_root), "%s/class", base);
    snprintf(hardware, sizeof(hardware), "%s/device", base);
    snprintf(entry, sizeof(entry), "%s/class/apple::kbd_backlight", base);
    assert(mkdir(fixture_root, 0700) == 0 && mkdir(hardware, 0700) == 0);
    snprintf(indicator, sizeof(indicator), "%s/indicator", base);
    snprintf(indicator_link, sizeof(indicator_link), "%s/class/input0::capslock", base);
    assert(mkdir(indicator, 0700) == 0 && symlink(indicator, indicator_link) == 0);
    put(indicator, "max_brightness", "1\n");
    put(indicator, "brightness", "0\n");
    uint32_t level = 77;
    assert(AuroraGetKeyboardBacklight(&level) == -1 && level == 77);
    assert(symlink(hardware, entry) == 0);
    put(hardware, "max_brightness", "4095\n");
    put(hardware, "brightness", "2048\n");
    assert(AuroraGetKeyboardBacklight(&level) == 0 && level == 128);
    assert(AuroraSetKeyboardBacklight(128) == 0);
    int dir = open(hardware, O_DIRECTORY | O_RDONLY);
    uint32_t actual;
    assert(dir >= 0 && read_number(dir, "brightness", &actual) == 0 && actual == 2056);
    assert(AuroraSetKeyboardBacklight(256) == -1);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == 2056);
    put(hardware, "brightness", "0\n");
    assert(AuroraGetKeyboardBacklight(&level) == 0 && level == 0);
    assert(AuroraSetKeyboardBacklight(255) == 0);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == 4095);
    assert(AuroraGetKeyboardBacklight(NULL) == -1);
    const char *invalid[] = {"", "garbage\n", "-1\n", "1x\n", "4294967296\n", "0\n"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        put(hardware, "max_brightness", invalid[i]);
        level = 77;
        assert(AuroraGetKeyboardBacklight(&level) == -1 && level == 77);
        assert(AuroraSetKeyboardBacklight(128) == -1);
    }
    put(hardware, "max_brightness", "4294967295\n");
    assert(AuroraSetKeyboardBacklight(255) == 0);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == UINT32_MAX);
    put(hardware, "max_brightness", "4095\n");
    put(hardware, "brightness", "4096\n");
    assert(AuroraGetKeyboardBacklight(&level) == -1);
    put(hardware, "brightness", "2048\n");
    snprintf(path, sizeof(path), "%s/class/apple::kbd_backlight_1", base);
    assert(symlink(hardware, path) == 0);
    assert(AuroraSetKeyboardBacklight(128) == -1 && errno == EBUSY);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/brightness", hardware);
    assert(unlink(path) == 0);
    assert(AuroraSetKeyboardBacklight(128) == -1);
    close(dir);
    snprintf(path, sizeof(path), "%s/max_brightness", hardware);
    assert(unlink(path) == 0 && unlink(entry) == 0);
    assert(unlink(indicator_link) == 0);
    snprintf(path, sizeof(path), "%s/brightness", indicator);
    FILE *indicator_state = fopen(path, "r"); unsigned unchanged = 99;
    assert(indicator_state && fscanf(indicator_state, "%u", &unchanged) == 1 && unchanged == 0);
    fclose(indicator_state); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/max_brightness", indicator); assert(unlink(path) == 0);
    assert(rmdir(indicator) == 0 && rmdir(hardware) == 0 && rmdir(fixture_root) == 0 && rmdir(base) == 0);
    puts("AURORA_KEYBOARD_LIGHT_TEST_PASS synthetic_sysfs=1 real_hardware=0");
    return 0;
}
