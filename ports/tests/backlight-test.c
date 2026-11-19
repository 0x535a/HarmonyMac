/* SPDX-License-Identifier: GPL-3.0-only
 * Synthetic sysfs tests only; never accesses a real backlight or display.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#include <limits.h>
static char fixture_root[PATH_MAX];
#define AURORA_BACKLIGHT_ROOT fixture_root
#include "../hardware/backlight.c"

static void put(const char *base, const char *name, const char *text)
{
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/%s", base, name) < (int)sizeof(path));
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

int main(void)
{
    char temporary[] = "/tmp/aurora-backlight-XXXXXX";
    char *base = mkdtemp(temporary);
    assert(base);
    char hardware[128], entry[128], path[PATH_MAX];
    snprintf(fixture_root, sizeof(fixture_root), "%s/class", base);
    snprintf(hardware, sizeof(hardware), "%s/device", base);
    snprintf(entry, sizeof(entry), "%s/class/panel", base);
    assert(mkdir(fixture_root, 0700) == 0 && mkdir(hardware, 0700) == 0);
    uint32_t level = 77;
    assert(AuroraGetBacklight(&level) == -1 && level == 77);
    assert(symlink(hardware, entry) == 0);
    put(hardware, "max_brightness", "4095\n");
    put(hardware, "brightness", "2048\n");
    assert(AuroraGetBacklight(&level) == 0 && level == 128);
    assert(AuroraSetBacklight(128) == 0);
    int dir = open(hardware, O_DIRECTORY | O_RDONLY);
    uint32_t actual;
    assert(dir >= 0 && read_number(dir, "brightness", &actual) == 0 && actual == 2056);
    assert(AuroraSetBacklight(256) == -1);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == 2056);
    put(hardware, "brightness", "0\n");
    assert(AuroraGetBacklight(&level) == 0 && level == 0);
    assert(AuroraSetBacklight(255) == 0);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == 4095);
    assert(AuroraGetBacklight(NULL) == -1);
    const char *invalid[] = {"", "garbage\n", "-1\n", "1x\n", "4294967296\n", "0\n"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        put(hardware, "max_brightness", invalid[i]);
        level = 77;
        assert(AuroraGetBacklight(&level) == -1 && level == 77);
        assert(AuroraSetBacklight(128) == -1);
    }
    put(hardware, "max_brightness", "4294967295\n");
    assert(AuroraSetBacklight(255) == 0);
    assert(read_number(dir, "brightness", &actual) == 0 && actual == UINT32_MAX);
    put(hardware, "max_brightness", "4095\n");
    put(hardware, "brightness", "4096\n");
    assert(AuroraGetBacklight(&level) == -1);
    put(hardware, "brightness", "2048\n");
    snprintf(path, sizeof(path), "%s/class/second-panel", base);
    assert(symlink(hardware, path) == 0);
    assert(AuroraSetBacklight(128) == -1 && errno == EBUSY);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/brightness", hardware);
    assert(unlink(path) == 0);
    assert(AuroraSetBacklight(128) == -1);
    close(dir);
    snprintf(path, sizeof(path), "%s/max_brightness", hardware);
    assert(unlink(path) == 0 && unlink(entry) == 0);
    assert(rmdir(hardware) == 0 && rmdir(fixture_root) == 0 && rmdir(base) == 0);
    puts("AURORA_BACKLIGHT_TEST_PASS synthetic_sysfs=1 real_hardware=0");
    return 0;
}
