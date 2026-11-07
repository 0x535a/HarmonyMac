#define _GNU_SOURCE
#include "../../../l410/dmabuf-ion/dmabuf_alloc.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "DMABUF_PROBE_FAIL %s errno=%d\n", what, errno);
        _exit(2);
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    check(DmabufHeapOpen("protect_heap") == -1 && errno == EOPNOTSUPP, "protected heap is not downgraded");
    int heap = DmabufHeapOpen("system_heap");
    check(heap >= 0, "open system heap");
    DmabufHeapBuffer buffer = {.fd = (unsigned int)-1, .size = 4096};
    SetOwnerIdForHeapFlags(&buffer, DMA_OWNER_GPU);
    check(DmabufHeapBufferAlloc(heap, &buffer) == 0, "allocate");
    unsigned char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
    unsigned char *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
    check(a != MAP_FAILED && b != MAP_FAILED, "map aliases");
    check(DmabufHeapBufferSyncStart(buffer.fd, DMA_BUF_HEAP_BUF_SYNC_RW) == 0, "sync begin");
    memset(a, 0x5a, 4096);
    check(b[17] == 0x5a, "coherent read");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        b[17] = 0xa5;
        _exit(0);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "child status");
    check(a[17] == 0xa5, "shared child write");
    check(DmabufHeapBufferSyncEnd(buffer.fd, DMA_BUF_HEAP_BUF_SYNC_RW) == 0, "sync end");
    munmap(a, 4096);
    munmap(b, 4096);
    check(DmabufHeapBufferFree(&buffer) == 0 && buffer.fd == (unsigned int)-1, "free");
    DmabufHeapClose(heap);
    printf("DMABUF_HEAP_PASS page=%ld allocation/mmap/sync/fork/protected-rejection\n", sysconf(_SC_PAGESIZE));
    return 0;
}
