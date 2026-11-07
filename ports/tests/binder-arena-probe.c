#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <poll.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok, const char *name)
{ if (!ok) { fprintf(stderr, "BINDER_ARENA_FAIL %s errno=%d\n", name, errno); exit(2); } }
static sigjmp_buf fault_return;
static void bus_handler(int signal_number) { siglongjmp(fault_return, signal_number); }
static void expect_sigbus(uintptr_t address)
{
    /* Binder VMAs are VM_DONTCOPY, so test faults in the owning process. */
    struct sigaction action = {.sa_handler = bus_handler}, previous;
    sigemptyset(&action.sa_mask);
    check(!sigaction(SIGBUS, &action, &previous), "install fault-check handler");
    int fault = sigsetjmp(fault_return, 1);
    if (!fault) {
        volatile unsigned char byte = *(volatile unsigned char *)address;
        (void)byte;
        check(0, "unallocated arena or padding was accessible");
    }
    check(fault == SIGBUS, "unallocated arena or padding must SIGBUS");
    check(!sigaction(SIGBUS, &previous, NULL), "restore fault-check handler");
}
static void one_case(unsigned int number, size_t offset, size_t length, int accepted)
{
    struct binderfs_device device = {};
    snprintf(device.name, sizeof(device.name), "arena-%u", number);
    int control = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    check(control >= 0 && !ioctl(control, BINDER_CTL_ADD, &device), "create isolated binder");
    close(control);
    char path[128]; snprintf(path, sizeof(path), "/dev/binderfs/%s", device.name);
    int server = open(path, O_RDWR | O_CLOEXEC);
    check(server >= 0, "server open");
    size_t reserve_size = length + 65536;
    void *reservation = mmap(NULL, reserve_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(reservation != MAP_FAILED, "reserve address range");
    uintptr_t address = (((uintptr_t)reservation + 16383) & ~(uintptr_t)16383) + offset;
    void *mapping = mmap((void *)address, length, PROT_READ, MAP_SHARED | MAP_FIXED, server, 0);
    if (!accepted) {
        check(mapping == MAP_FAILED && errno == EINVAL, "reject VMA with no complete native page");
        munmap(reservation, reserve_size); close(server); unlink(path);
        printf("BINDER_ARENA_REJECT offset=%zu length=%zu\n", offset, length); return;
    }
    check(mapping == (void *)address, "exact requested mapping");
    uintptr_t arena_start = (address + 16383) & ~(uintptr_t)16383;
    size_t arena_size = (address + length - arena_start) & ~(size_t)16383;
    expect_sigbus(arena_start);
    if (arena_start > address) expect_sigbus(address);
    if (arena_start + arena_size < address + length) expect_sigbus(arena_start + arena_size);
    size_t payload_size = arena_size > 65536 ? 65536 : 512;
    check(!ioctl(server, BINDER_SET_CONTEXT_MGR, 0), "context manager");
    pid_t child = fork(); check(child >= 0, "client fork");
    if (!child) {
        close(server);
        int client = open(path, O_RDWR | O_CLOEXEC);
        check(client >= 0, "client open");
        unsigned char *payload = malloc(payload_size);
        check(payload != NULL, "payload allocation");
        for (size_t n = 0; n < payload_size; ++n) payload[n] = (unsigned char)(n * 17 + number);
        struct { uint32_t command; struct binder_transaction_data data; } __attribute__((packed)) request = {};
        request.command = BC_TRANSACTION;
        request.data.target.handle = 0; request.data.code = number + 1;
        request.data.flags = TF_ONE_WAY;
        request.data.data_size = payload_size;
        request.data.data.ptr.buffer = (binder_uintptr_t)payload;
        struct binder_write_read bwr = {.write_size = sizeof(request), .write_buffer = (binder_uintptr_t)&request};
        check(!ioctl(client, BINDER_WRITE_READ, &bwr) && bwr.write_consumed == sizeof(request), "send payload");
        close(client); free(payload); _exit(0);
    }
    uint32_t enter = BC_ENTER_LOOPER;
    unsigned char response[4096];
    int received = 0;
    for (int attempt = 0; attempt < 8 && !received; ++attempt) {
        struct binder_write_read bwr = {.write_size = attempt ? 0 : sizeof(enter),
            .write_buffer = (binder_uintptr_t)&enter, .read_size = sizeof(response), .read_buffer = (binder_uintptr_t)response};
        check(!ioctl(server, BINDER_WRITE_READ, &bwr), "receive payload");
        for (size_t cursor = 0; cursor + 4 <= bwr.read_consumed;) {
            uint32_t command; memcpy(&command, response + cursor, 4); cursor += 4;
            size_t size = _IOC_SIZE(command);
            check(cursor + size <= bwr.read_consumed, "response framing");
            if (command == BR_TRANSACTION) {
                struct binder_transaction_data transaction;
                memcpy(&transaction, response + cursor, sizeof(transaction));
                check(transaction.code == number + 1 && transaction.data_size == payload_size, "payload shape");
                uintptr_t start = transaction.data.ptr.buffer;
                check(start >= arena_start && start + payload_size <= arena_start + arena_size, "payload inside native arena");
                const unsigned char *payload = (void *)start;
                for (size_t n = 0; n < payload_size; ++n)
                    check(payload[n] == (unsigned char)(n * 17 + number), "cross-page payload bytes");
                struct { uint32_t command; binder_uintptr_t pointer; } __attribute__((packed)) release = {BC_FREE_BUFFER, start};
                struct binder_write_read free_buffer = {.write_size = sizeof(release), .write_buffer = (binder_uintptr_t)&release};
                check(!ioctl(server, BINDER_WRITE_READ, &free_buffer), "release transaction buffer");
                received = 1;
            }
            cursor += size;
        }
    }
    check(received, "transaction delivery");
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "client exit");
    munmap(reservation, reserve_size); close(server); unlink(path);
    printf("BINDER_ARENA_PASS offset=%zu length=%zu arena=%zu payload=%zu\n", offset, length, arena_size, payload_size);
}
int main(void)
{
    setbuf(stdout, NULL); alarm(30);
    long page = sysconf(_SC_PAGESIZE);
    if (page == 16384) {
        one_case(0, 0, 16384, 1);
        one_case(1, 0, 1024 * 1024, 1);
        puts("BINDER_NATIVE_ARENA_NATIVE_PASS cases=2 page=16384");
        return 0;
    }
    check(page == 4096, "run mixed-granule 4 KiB matrix");
    one_case(0, 0, 4096, 0);
    one_case(1, 4096, 16384, 0);
    one_case(2, 0, 16384, 1);
    one_case(3, 4096, 32768, 1);
    for (unsigned n = 0; n < 4; ++n) one_case(4 + n, n * 4096, 1024 * 1024 - 8192, 1);
    puts("BINDER_NATIVE_ARENA_MATRIX_PASS cases=8 page=4096");
    return 0;
}
