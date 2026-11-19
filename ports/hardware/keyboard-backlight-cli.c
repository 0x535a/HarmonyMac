/* SPDX-License-Identifier: GPL-3.0-only */
#include "keyboard-backlight.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "set")) {
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (errno || argv[2][0] < '0' || argv[2][0] > '9' || !end || *end || value > 255) {
            fputs("keyboard-backlight: level must be 0..255\n", stderr);
            return 2;
        }
        if (AuroraSetKeyboardBacklight((uint32_t)value)) {
            fprintf(stderr, "keyboard-backlight: set failed errno=%d (%s)\n", errno, strerror(errno));
            return 1;
        }
    } else if (argc != 2 || strcmp(argv[1], "get")) {
        fputs("usage: keyboard-backlight get | set LEVEL_0_TO_255\n", stderr);
        return 2;
    }
    uint32_t value;
    if (AuroraGetKeyboardBacklight(&value)) {
        fprintf(stderr, "keyboard-backlight: get failed errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("KEYBOARD_BACKLIGHT level=%u scale=255\n", value);
    return 0;
}
