#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int counter;

static void *worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 10000; ++i) {
        pthread_mutex_lock(&lock);
        ++counter;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(void)
{
    setbuf(stdout, NULL);
    long page = sysconf(_SC_PAGESIZE);
    printf("OHOS_RUNTIME_PROBE page=%ld\n", page);
    if (page != 4096) return 2;
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        if (pthread_create(&threads[i], NULL, worker, NULL)) return 3;
    for (int i = 0; i < 2; ++i)
        if (pthread_join(threads[i], NULL)) return 4;
    if (counter != 20000) return 5;
    int fd = syscall(SYS_memfd_create, "hmos-runtime-probe", 0);
    if (fd < 0 || ftruncate(fd, page * 2)) return 6;
    uint32_t *shared = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) return 7;
    shared[0] = 0x12345678;
    pid_t child = fork();
    if (child < 0) return 8;
    if (!child) {
        if (shared[0] != 0x12345678) _exit(10);
        shared[0] = 0x87654321;
        _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 9;
    if (shared[0] != 0x87654321) return 10;
    if (mprotect((char *)shared + page, page, PROT_NONE)) return 11;
    munmap(shared, page * 2);
    close(fd);
    puts("OHOS_RUNTIME_PROBE_PASS pthread TLS/mutex, fork, memfd, shared mapping, 4k protection");
    return 0;
}
