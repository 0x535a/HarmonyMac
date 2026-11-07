/* Bisect harness: reproduce init's pre-exec child state, then execve the
 * original OHOS shell. Each flag applies one element of the state observed in
 * init's failing child (strace of pid 91):
 *   -t  SET_TOKENID ioctl on /dev/access_token_id (native-type token)
 *   -s  prctl(PR_SET_SECUREBITS, KEEP_CAPS|KEEP_CAPS_LOCKED)
 *   -b  PR_CAPBSET_DROP everything except the five service caps
 *   -c  capset eff=perm=inh = DAC_OVERRIDE|DAC_READ_SEARCH|SYS_PTRACE|
 *       SYS_ADMIN|SYS_BOOT
 * The probe first dyntransitions itself to u:r:init:s0 (allowed under the
 * permissive diagnostic policy) so the final execve reproduces the exact
 * init -> limit_domain transition of the real service start. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SET_TOKENID _IOW('A', 2, unsigned long long)
#define NATIVE_TOKEN (0x28000000ULL | 0x2001ULL) /* version=1 type=NATIVE id */
#ifndef SECBIT_KEEP_CAPS
#define SECBIT_KEEP_CAPS (1u << 4)
#endif
#ifndef SECBIT_KEEP_CAPS_LOCKED
#define SECBIT_KEEP_CAPS_LOCKED (1u << 5)
#endif

#define V3_CAPS 5 /* CAP_DAC_OVERRIDE=1, CAP_DAC_READ_SEARCH=2 */
#define KEEP5_MASK0 ((1u << 1) | (1u << 2) | (1u << 19) | (1u << 21) | (1u << 22))

struct cap_header { uint32_t version; int pid; };
struct cap_data { uint32_t effective, permitted, inheritable; };

static int write_attr(const char *file, const char *value)
{
    int fd = open(file, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t r = write(fd, value, strlen(value));
    close(fd);
    return r < 0 ? -1 : 0;
}

static void show_ctx(const char *when)
{
    char ctx[256] = {0};
    int fd = open("/proc/self/attr/current", O_RDONLY);
    if (fd >= 0) { read(fd, ctx, sizeof ctx - 1); close(fd); }
    printf("EXEC_CHILD_PROBE ctx-%s=%s", when, ctx[0] ? ctx : "(none)\n");
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    int do_tok = 0, do_sec = 0, do_bound = 0, do_capset = 0, exact = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) do_tok = 1;
        else if (!strcmp(argv[i], "-s")) do_sec = 1;
        else if (!strcmp(argv[i], "-b")) do_bound = 1;
        else if (!strcmp(argv[i], "-c")) do_capset = 1;
        else if (!strcmp(argv[i], "-x")) exact = 1;
    }
    if (exact) { do_tok = do_sec = do_bound = do_capset = 1; }
    printf("EXEC_CHILD_PROBE begin t=%d s=%d b=%d c=%d x=%d pid=%d pagesz=%lu\n",
           do_tok, do_sec, do_bound, do_capset, exact, getpid(),
           (unsigned long)getauxval(AT_PAGESZ));
    show_ctx("before");
    if (write_attr("/proc/self/attr/current", "u:r:init:s0") == 0)
        printf("EXEC_CHILD_PROBE dyntrans=ok\n");
    else
        printf("EXEC_CHILD_PROBE dyntrans=failed errno=%d\n", errno);
    show_ctx("after");

    if (do_tok) {
        int fd = open("/dev/access_token_id", O_RDWR);
        if (fd < 0) {
            printf("EXEC_CHILD_PROBE token_open errno=%d\n", errno);
        } else {
            uint64_t token = NATIVE_TOKEN;
            int r = ioctl(fd, SET_TOKENID, &token);
            printf("EXEC_CHILD_PROBE set_token r=%d errno=%d\n", r, r < 0 ? errno : 0);
            close(fd);
        }
    }
    if (do_sec) {
        int r = prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS | SECBIT_KEEP_CAPS_LOCKED);
        printf("EXEC_CHILD_PROBE securebits r=%d errno=%d\n", r, r < 0 ? errno : 0);
    }
    if (do_bound) {
        int last_fail = 0;
        /* Mirror init exactly: drop every bounding cap except the service's. */
        for (int cap = 0; cap <= 40; cap++) {
            if (cap < 32 && (KEEP5_MASK0 & (1u << cap))) continue;
            if (prctl(PR_CAPBSET_DROP, cap) != 0) last_fail = errno;
        }
        printf("EXEC_CHILD_PROBE capbset_drop last_fail=%d\n", last_fail);
    }
    if (do_capset) {
        struct cap_header h = {0x20080522, 0}; /* _LINUX_CAPABILITY_VERSION_3 */
        struct cap_data d[2];
        memset(d, 0, sizeof d);
        d[0].effective = d[0].permitted = d[0].inheritable = KEEP5_MASK0;
        int r = syscall(SYS_capset, &h, d);
        printf("EXEC_CHILD_PROBE capset r=%d errno=%d\n", r, r < 0 ? errno : 0);
    }

    printf("EXEC_CHILD_PROBE execve now\n");
    if (exact) {
        setgid(0);
        /* init passes the script as argv[1] and four environment vars. */
        char *child_argv[] = {"/system/bin/sh", "/system/bin/aurora/probe-target.sh", NULL};
        char *child_envp[] = {"PATH=/system/bin:/bin", "HOME=/",
                              "SHELL=/system/bin/sh", "TMPDIR=/tmp", NULL};
        execve("/system/bin/sh", child_argv, child_envp);
    } else {
        char *child_argv[] = {"/system/bin/sh", "-c", "echo EXEC_CHILD_TARGET_OK", NULL};
        char *child_envp[] = {"PATH=/system/bin:/bin", "HOME=/", "SHELL=/system/bin/sh", NULL};
        execve("/system/bin/sh", child_argv, child_envp);
    }
    printf("EXEC_CHILD_PROBE execve_failed errno=%d (%s)\n", errno, strerror(errno));
    return 1;
}
