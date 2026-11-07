#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("PAGE_PROBE sysconf=%ld auxv=%lu pid=%ld\n",
           sysconf(_SC_PAGESIZE), getauxval(AT_PAGESZ), (long)getpid());
    fflush(stdout);
    void *p = mmap(NULL, 32768, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    errno = 0;
    int result = mprotect((char *)p + 4096, 4096, PROT_READ);
    printf("PAGE_PROBE mprotect_4k=%d errno=%d\n", result, errno);
    munmap(p, 32768);
    if (argc < 2)
        return 0;
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 2;
    }
    if (!child) {
        execv(argv[1], argv + 1);
        perror("execv");
        _exit(126);
    }
    int status;
    if (waitpid(child, &status, 0) != child) {
        perror("waitpid");
        return 2;
    }
    if (WIFSIGNALED(status)) {
        printf("PAGE_PROBE child_signal=%d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    printf("PAGE_PROBE child_exit=%d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
