#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define GET_TOKEN _IOR('A', 1, unsigned long long)
#define SET_TOKEN _IOW('A', 2, unsigned long long)
#define GET_FIRST _IOR('A', 3, unsigned long long)
#define SET_FIRST _IOW('A', 4, unsigned long long)
struct permission_set { uint32_t token; uint32_t perm[64]; };
struct permission_item { uint32_t token; uint32_t operation; bool granted; };
#define ADD_PERMS _IOW('A', 5, struct permission_set)
#define REMOVE_PERMS _IOW('A', 6, uint32_t)
#define GET_PERM _IOW('A', 7, struct permission_item)
#define SET_PERM _IOW('A', 8, struct permission_item)
#define CHILD_TOKEN UINT64_C(0x12345678)
#define FIRST_TOKEN UINT64_C(0x23456789)

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "IPC_PROBE_FAIL %s errno=%d\n", message, errno);
        exit(2);
    }
}

static int binder_open(void)
{
    int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = open("/dev/binderfs/binder", O_RDWR | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) {
            int control = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
            check(control >= 0, "open binderfs control");
            struct binderfs_device device = {.name = "binder"};
            check(ioctl(control, BINDER_CTL_ADD, &device) == 0, "allocate binderfs device");
            close(control);
            fd = open("/dev/binderfs/binder", O_RDWR | O_CLOEXEC);
        }
    }
    check(fd >= 0, "open binder");
    struct binder_version version = {0};
    check(ioctl(fd, BINDER_VERSION, &version) == 0, "version");
    void *mapping = mmap(NULL, 1024 * 1024, PROT_READ,
                         MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    check(mapping != MAP_FAILED, "binder mmap");
    return fd;
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(20);
    int tokenfd = open("/dev/access_token_id", O_RDWR | O_CLOEXEC);
    check(tokenfd >= 0, "open token device");
    uint64_t token = 100, first = FIRST_TOKEN;
    check(ioctl(tokenfd, SET_TOKEN, &token) == 0, "set parent token");
    check(ioctl(tokenfd, SET_FIRST, &first) == 0, "set first token");
    struct permission_set permissions = {.token = 100, .perm = {[0] = 1U << 7}};
    struct permission_item permission = {.token = 100, .operation = 7, .granted = false};
    /* The OH 6.0 release permits bitmap changes only for AccessToken UID 3020.
     * Root is intentionally not granted that permission by this ABI. */
    check(ioctl(tokenfd, ADD_PERMS, &permissions) == -1 && errno == EPERM,
          "root cannot modify AccessToken service bitmap");
    check(setresuid(3020, 3020, 0) == 0, "assume AccessToken service identity");
    check(ioctl(tokenfd, ADD_PERMS, &permissions) == 0, "add permission bitmap");
    check(ioctl(tokenfd, GET_PERM, &permission) == 1, "read granted permission");
    check(ioctl(tokenfd, SET_PERM, &permission) == 0, "revoke permission");
    check(ioctl(tokenfd, GET_PERM, &permission) == 0, "read denied permission");
    uint32_t permission_token = 100;
    check(ioctl(tokenfd, REMOVE_PERMS, &permission_token) == 0, "remove permission bitmap");
    check(ioctl(tokenfd, GET_PERM, &permission) == -1 && errno == ENODATA,
          "removed permission absent");
    check(setresuid(0, 0, 0) == 0, "restore root after bitmap test");
    pid_t unprivileged = fork();
    check(unprivileged >= 0, "fork permission check");
    if (!unprivileged) {
        check(setresuid(2000, 2000, 2000) == 0, "drop uid for permission check");
        check(ioctl(tokenfd, SET_TOKEN, &token) == -1 && errno == EPERM,
              "unprivileged token assignment rejected");
        check(ioctl(tokenfd, ADD_PERMS, &permissions) == -1 && errno == EPERM,
              "unprivileged permission assignment rejected");
        _exit(0);
    }
    int permission_status;
    check(waitpid(unprivileged, &permission_status, 0) == unprivileged &&
          WIFEXITED(permission_status) && WEXITSTATUS(permission_status) == 0,
          "permission rejection child");
    int fd = binder_open();
    struct binder_feature_set features = {0};
    check(ioctl(fd, BINDER_FEATURE_SET, &features) == 0, "feature set");
    check((features.feature_set & 5) == 5, "feature bits");
    struct flat_binder_object manager = {.hdr.type = BINDER_TYPE_BINDER,
        .flags = FLAT_BINDER_FLAG_ACCEPTS_FDS};
    check(ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, &manager) == 0, "context manager accepting FDs");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        close(fd);
        uint64_t inherited = 0;
        check(ioctl(tokenfd, GET_TOKEN, &inherited) == 0 && inherited == 100,
              "fork token inheritance");
        check(ioctl(tokenfd, GET_FIRST, &inherited) == 0 && inherited == FIRST_TOKEN,
              "fork first-token inheritance");
        token = CHILD_TOKEN;
        check(ioctl(tokenfd, SET_TOKEN, &token) == 0, "child identity");
        int client = binder_open();
        int sharedfd = syscall(SYS_memfd_create, "hmos-binder-fd-probe", 0);
        uint32_t fd_content = 0x46445348;
        check(sharedfd >= 0 && write(sharedfd, &fd_content, sizeof(fd_content)) == sizeof(fd_content),
              "prepare shared FD");
        struct {
            uint32_t value;
            uint32_t padding;
            struct flat_binder_object shared;
        } value = {.value = 0x484d4f53, .shared = {.hdr.type = BINDER_TYPE_FD, .handle = sharedfd}};
        binder_size_t offsets[] = {8};
        struct {
            uint32_t cmd;
            struct binder_transaction_data txn;
        } __attribute__((packed)) request = {
            .cmd = BC_TRANSACTION,
            .txn = {.target.handle = 0, .code = 17, .flags = TF_ONE_WAY,
                    .data_size = sizeof(value),
                    .offsets_size = sizeof(offsets),
                    .data.ptr.buffer = (binder_uintptr_t)&value,
                    .data.ptr.offsets = (binder_uintptr_t)offsets}
        };
        struct binder_write_read bwr = {.write_size = sizeof(request),
            .write_buffer = (binder_uintptr_t)&request};
        check(ioctl(client, BINDER_WRITE_READ, &bwr) == 0, "send transaction");
        check(bwr.write_consumed == sizeof(request), "transaction consumed");
        sleep(2);
        _exit(0);
    }
    uint32_t enter = BC_ENTER_LOOPER;
    unsigned char response[1024];
    int received = 0;
    for (int attempt = 0; attempt < 10 && !received; ++attempt) {
        struct binder_write_read bwr = {
            .write_size = attempt ? 0 : sizeof(enter),
            .write_buffer = (binder_uintptr_t)&enter,
            .read_size = sizeof(response), .read_buffer = (binder_uintptr_t)response};
        check(ioctl(fd, BINDER_WRITE_READ, &bwr) == 0, "receive transaction");
        size_t cursor = 0;
        while (cursor + 4 <= bwr.read_consumed) {
            uint32_t cmd;
            memcpy(&cmd, response + cursor, 4);
            cursor += 4;
            size_t bytes = _IOC_SIZE(cmd);
            check(cursor + bytes <= bwr.read_consumed, "response size");
            if (cmd == BR_TRANSACTION) {
                struct binder_transaction_data txn;
                memcpy(&txn, response + cursor, sizeof(txn));
                check(txn.code == 17 && txn.data_size == 8 + sizeof(struct flat_binder_object), "payload shape");
                check(*(uint32_t *)(uintptr_t)txn.data.ptr.buffer == 0x484d4f53,
                      "payload content");
                struct flat_binder_object transferred;
                memcpy(&transferred, (void *)(uintptr_t)(txn.data.ptr.buffer + 8), sizeof(transferred));
                uint32_t fd_content = 0;
                check(transferred.hdr.type == BINDER_TYPE_FD &&
                      pread(transferred.handle, &fd_content, sizeof(fd_content), 0) == sizeof(fd_content) &&
                      fd_content == 0x46445348, "transferred FD content");
                close(transferred.handle);
                struct access_token incoming = {0};
                check(ioctl(fd, BINDER_GET_ACCESS_TOKEN, &incoming) == 0,
                      "incoming token ioctl");
                check(incoming.sender_tokenid == CHILD_TOKEN &&
                      incoming.first_tokenid == FIRST_TOKEN, "incoming identity");
                struct binder_sender_info sender = {0};
                check(ioctl(fd, BINDER_GET_SENDER_INFO, &sender) == 0,
                      "incoming sender-info ioctl");
                check(sender.sender_pid_nr == (uint64_t)child &&
                      sender.tokens.sender_tokenid == CHILD_TOKEN, "sender pid identity");
                received = 1;
            }
            cursor += bytes;
        }
    }
    check(received, "no transaction");
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "child status");
    token = 0;
    check(ioctl(tokenfd, GET_TOKEN, &token) == 0 && token == 100,
          "child changes do not alter parent token");
    puts("IPC_PROBE_PASS permissions, privilege-rejection, fork-inheritance, token-isolation, transaction, payload, FD-transfer, caller-token, first-token, sender-pid");
    return 0;
}
