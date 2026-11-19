// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s MODULE PARAMETERS\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    long result = syscall(SYS_finit_module, fd, argv[2], 0);
    if (result < 0)
        perror("appledrm finit_module");
    close(fd);
    return result < 0;
}
