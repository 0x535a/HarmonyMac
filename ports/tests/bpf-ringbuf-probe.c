/* Exercise the public BPF ring-buffer mmap ABI, including wrap and permissions.
 * Build for both the original OHOS ABI and native Linux diagnostics. */
#include <errno.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define INS(code_, dst_, src_, off_, imm_) \
    ((struct bpf_insn){.code=(code_), .dst_reg=(dst_), .src_reg=(src_), .off=(off_), .imm=(imm_)})

/* Stable UAPI value; the product SDK predates this map's header definition. */
#define USER_RINGBUF_MAP_TYPE 31

static int fail(const char *stage)
{
    printf("BPF_RING_FAIL stage=%s errno=%d message=%s\n", stage, errno, strerror(errno));
    return 2;
}

int main(void)
{
    const size_t capacity = 65536;
    long page = sysconf(_SC_PAGESIZE);
    setbuf(stdout, NULL);
    printf("BPF_RING_ENV sysconf_page=%ld auxv_page=%lu\n", page, getauxval(AT_PAGESZ));
    if (page != 4096 && page != 16384) return 2;
    union bpf_attr attr = {.map_type=BPF_MAP_TYPE_RINGBUF, .max_entries=capacity};
    int map = syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
    if (map < 0) return fail("map-create");
    uint64_t *consumer = mmap(NULL, page, PROT_READ|PROT_WRITE, MAP_SHARED, map, 0);
    if (consumer == MAP_FAILED) return fail("consumer-mmap");
    size_t read_size = page + 2 * capacity;
    uint64_t *producer = mmap(NULL, read_size, PROT_READ, MAP_SHARED, map, page);
    if (producer == MAP_FAILED) return fail("producer-data-mmap");
    const unsigned char *data = (const unsigned char *)producer + page;
    printf("BPF_RING_MAPS consumer=%p producer=%p initial_consumer=%llu initial_producer=%llu\n",
           consumer, producer, (unsigned long long)*consumer, (unsigned long long)*producer);
    errno = 0;
    void *bad = mmap(NULL, page, PROT_READ|PROT_WRITE, MAP_SHARED, map, page);
    if (bad != MAP_FAILED) { munmap(bad, page); errno=0; return fail("producer-write-allowed"); }
    printf("BPF_RING_PRODUCER_WRITE_REJECT errno=%d\n", errno);
    bad = mmap(NULL, 2*page, PROT_READ|PROT_WRITE, MAP_SHARED, map, 0);
    if (bad != MAP_FAILED) { munmap(bad, 2*page); errno=0; return fail("oversize-consumer-write-allowed"); }
    if (mprotect(producer, read_size, PROT_READ|PROT_WRITE) == 0) {
        errno=0; return fail("producer-mprotect-write-allowed");
    }
    bad = mmap(NULL, page, PROT_READ, MAP_SHARED, map, 2*page+2*capacity);
    if (bad != MAP_FAILED) { munmap(bad, page); errno=0; return fail("out-of-range-allowed"); }
    bad = mmap(NULL, page, PROT_READ|PROT_WRITE, MAP_SHARED, map, 2*page);
    if (bad != MAP_FAILED) { munmap(bad, page); errno=0; return fail("data-write-allowed"); }
    for (size_t offset=0; offset<2*page+2*capacity; offset+=page) {
        unsigned char *part = mmap(NULL, page, PROT_READ, MAP_SHARED, map, offset);
        if (part == MAP_FAILED) return fail("read-slice-mmap");
        volatile unsigned char touch = part[0] | part[page-1];
        (void)touch;
        munmap(part, page);
    }
    puts("BPF_RING_PERMISSIONS_PASS");
    /* 13-byte payload => 24-byte record: one record crosses the ring boundary. */
    struct bpf_insn instructions[] = {
        INS(BPF_ST|BPF_MEM|BPF_DW, BPF_REG_10, 0, -16, 0x12345678),
        INS(BPF_ST|BPF_MEM|BPF_DW, BPF_REG_10, 0, -8, 0x23456701),
        INS(BPF_LD|BPF_DW|BPF_IMM, BPF_REG_1, BPF_PSEUDO_MAP_FD, 0, map),
        INS(0, 0, 0, 0, 0),
        INS(BPF_ALU64|BPF_MOV|BPF_X, BPF_REG_2, BPF_REG_10, 0, 0),
        INS(BPF_ALU64|BPF_ADD|BPF_K, BPF_REG_2, 0, 0, -16),
        INS(BPF_ALU64|BPF_MOV|BPF_K, BPF_REG_3, 0, 0, 13),
        INS(BPF_ALU64|BPF_MOV|BPF_K, BPF_REG_4, 0, 0, 0),
        INS(BPF_JMP|BPF_CALL, 0, 0, 0, BPF_FUNC_ringbuf_output),
        INS(BPF_JMP|BPF_EXIT, 0, 0, 0, 0),
    };
    char verifier_log[8192] = {0};
    const char license[] = "GPL";
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
    attr.insn_cnt = sizeof(instructions)/sizeof(instructions[0]);
    attr.insns = (uintptr_t)instructions;
    attr.license = (uintptr_t)license;
    attr.log_buf = (uintptr_t)verifier_log;
    attr.log_size = sizeof(verifier_log);
    attr.log_level = 1;
    int program = syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
    if (program < 0) { puts(verifier_log); return fail("program-load"); }
    const uint64_t expected[] = {0x12345678, 0x23456701};
    unsigned char packet[64] = {0};
    unsigned crossing_records = 0;
    for (unsigned i=0; i<3000; ++i) {
        memset(&attr, 0, sizeof(attr));
        attr.test.prog_fd = program;
        attr.test.data_in = (uintptr_t)packet;
        attr.test.data_size_in = sizeof(packet);
        attr.test.repeat = 1;
        if (syscall(__NR_bpf, BPF_PROG_TEST_RUN, &attr, sizeof(attr)) || attr.test.retval)
            return fail("program-run");
        uint64_t start = (uint64_t)i*24;
        uint64_t end = __atomic_load_n(producer, __ATOMIC_ACQUIRE);
        uint32_t length;
        memcpy(&length, data+(start&(capacity-1)), sizeof(length));
        if (end != start+24 || length != 13 ||
            memcmp(data+(start&(capacity-1))+8, expected, 13)) {
            errno=0; return fail("record-data-or-position");
        }
        if ((start&(capacity-1))+24 > capacity) crossing_records++;
        __atomic_store_n(consumer, end, __ATOMIC_RELEASE);
    }
    if (crossing_records != 1) { errno=0; return fail("wrap-coverage"); }
    printf("BPF_RING_RECORDS_PASS count=3000 bytes=72000 boundary_crossings=%u\n", crossing_records);
    munmap(producer, read_size);
    munmap(consumer, page);
    close(program);
    close(map);
    puts("BPF_RING_PASS");
    memset(&attr, 0, sizeof(attr));
    attr.map_type = USER_RINGBUF_MAP_TYPE;
    attr.max_entries = capacity;
    map = syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
    if (map < 0) return fail("user-ring-map-create");
    consumer = mmap(NULL, page, PROT_READ, MAP_SHARED, map, 0);
    if (consumer == MAP_FAILED) return fail("user-ring-consumer-mmap");
    producer = mmap(NULL, read_size, PROT_READ|PROT_WRITE, MAP_SHARED, map, page);
    if (producer == MAP_FAILED) return fail("user-ring-producer-mmap");
    bad = mmap(NULL, page, PROT_READ|PROT_WRITE, MAP_SHARED, map, 0);
    if (bad != MAP_FAILED) { munmap(bad, page); errno=0; return fail("user-ring-consumer-write-allowed"); }
    if (mprotect(consumer, page, PROT_READ|PROT_WRITE) == 0) {
        errno=0; return fail("user-ring-consumer-mprotect-allowed");
    }
    volatile unsigned char *user_data = (volatile unsigned char *)producer + page;
    user_data[0] = 0x37;
    user_data[capacity-1] = 0x59;
    if (user_data[capacity] != 0x37 || user_data[2*capacity-1] != 0x59) {
        errno=0; return fail("user-ring-double-map-alias");
    }
    __atomic_store_n(producer, 24, __ATOMIC_RELEASE);
    if (*consumer != 0 || *producer != 24) { errno=0; return fail("user-ring-position-layout"); }
    munmap(producer, read_size);
    munmap(consumer, page);
    close(map);
    puts("BPF_USER_RING_MAPPING_PASS");
    return 0;
}
