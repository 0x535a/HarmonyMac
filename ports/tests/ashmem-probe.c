#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ashmem-uapi.h"

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "ASHMEM_PROBE_FAIL %s errno=%d\n", what, errno);
        exit(2);
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(15);
    long page = sysconf(_SC_PAGESIZE);
    check(page == 4096 || page == 16384, "process page size");
    size_t size = (size_t)page * 3;
    int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    check(fd >= 0, "open device");
    check(ioctl(fd, ASHMEM_SET_NAME, "aurora-ashmem-probe") == 0, "set name");
    char name[ASHMEM_NAME_LEN] = {0};
    check(ioctl(fd, ASHMEM_GET_NAME, name) == 0 && !strcmp(name, "aurora-ashmem-probe"), "get name");
    check(ioctl(fd, ASHMEM_SET_SIZE, size) == 0, "set size");
    check(ioctl(fd, ASHMEM_GET_SIZE) == (long)size, "get size");
    unsigned char *a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *b = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(a != MAP_FAILED && b != MAP_FAILED, "shared aliases");
    memset(a, 0xa5, size);
    check(b[4096] == 0xa5, "coherent aliases");
    unsigned char *offset = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
    check(offset != MAP_FAILED && offset[0] == 0xa5, "nonzero file offset");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        check(b[0] == 0xa5, "child shared read");
        b[0] = 0x5a;
        _exit(0);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "child status");
    check(a[0] == 0x5a, "child shared write");
    struct ashmem_pin range = {.offset = 4096, .len = 4096};
    check(ioctl(fd, ASHMEM_UNPIN, &range) == 0, "unpin middle 4k");
    check(ioctl(fd, ASHMEM_GET_PIN_STATUS, &range) == ASHMEM_IS_UNPINNED, "unpinned status");
    check(ioctl(fd, ASHMEM_PURGE_ALL_CACHES) >= 1, "explicit purge");
    for (unsigned int i = 4096; i < 8192; ++i) check(a[i] == 0, "purged bytes zeroed");
    check(a[0] == 0x5a && a[4095] == 0xa5 && a[8192] == 0xa5, "adjacent bytes retained");
    check(ioctl(fd, ASHMEM_PIN, &range) == ASHMEM_WAS_PURGED, "purge notification");
    check(ioctl(fd, ASHMEM_PIN, &range) == ASHMEM_NOT_PURGED, "purge state reset");
    check(ioctl(fd, ASHMEM_GET_PIN_STATUS, &range) == ASHMEM_IS_PINNED, "pinned status");
    close(fd);
    check(a[0] == 0x5a && b[8192] == 0xa5, "mappings outlive control fd");
    munmap(offset, page);
    munmap(a, size);
    munmap(b, size);

    fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    check(fd >= 0 && ioctl(fd, ASHMEM_SET_SIZE, page) == 0, "protection region");
    check(ioctl(fd, ASHMEM_SET_PROT_MASK, PROT_READ) == 0, "reduce protection");
    check(ioctl(fd, ASHMEM_SET_PROT_MASK, PROT_READ | PROT_WRITE) == -1 && errno == EINVAL,
          "protection cannot be raised");
    a = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    check(a != MAP_FAILED, "read-only mapping");
    check(mprotect(a, page, PROT_READ | PROT_WRITE) == -1, "mprotect cannot bypass mask");
    check(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED,
          "new writable mapping rejected");
    child = fork();
    check(child >= 0, "fork privilege test");
    if (!child) {
        check(setresuid(2000, 2000, 2000) == 0, "drop purge privilege");
        check(ioctl(fd, ASHMEM_PURGE_ALL_CACHES) == -1 && errno == EPERM, "unprivileged purge rejected");
        _exit(0);
    }
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "privilege result");
    munmap(a, page);
    close(fd);
    printf("ASHMEM_PROBE_PASS page=%ld shared/fork/offset/purge/protection/lifetime/privilege\n", page);
    return 0;
}
