/* Temporary diagnostic launcher reproducing hilogd's init socket inheritance.
 * Built for OHOS; no logging or service implementation is replaced.
 * Mode "dump": instead of exec'ing hilogd, keep the bound hilogInput socket
 * and echo every received datagram raw to stdout, bypassing the L410 hilogd
 * dict-compression pipeline that cannot be decoded offline in this fixture. */
#include <errno.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int dump_loop(int input)
{
    static unsigned char buffer[8192];
    setbuf(stdout, NULL);
    for (;;) {
        ssize_t size = recv(input, buffer, sizeof buffer, 0);
        if (size <= 0) {
            if (size < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            fprintf(stderr, "hilog dump recv=%zd errno=%d\n", size, size < 0 ? errno : 0);
            return 0;
        }
        fputs("===HILOGRAM===\n", stdout);
        if (fwrite(buffer, 1, (size_t)size, stdout) != (size_t)size)
            return 0;
        fputc('\n', stdout);
    }
}

int main(int argc, char **argv)
{
    const char *names[] = {"hilogInput", "hilogOutput", "hilogControl", "hilogControlPub"};
    int fds[4];
    for (unsigned int n = 0; n < 4; ++n) {
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        snprintf(address.sun_path, sizeof(address.sun_path), "/dev/unix/socket/%s", names[n]);
        int fd = socket(AF_UNIX, n == 0 ? SOCK_DGRAM : SOCK_SEQPACKET, 0);
        int enabled = 1;
        if (fd < 0 || (n < 2 && setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)))) {
            perror("hilog socket"); return 2;
        }
        unlink(address.sun_path);
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
            chmod(address.sun_path, n == 0 ? 0222 : 0666) || (n && listen(fd, 16))) {
            perror(address.sun_path); return 2;
        }
        char key[80], value[32];
        snprintf(key, sizeof(key), "OHOS_SOCKET_%s", names[n]);
        snprintf(value, sizeof(value), "%d", fd);
        if (setenv(key, value, 1)) return 2;
        fds[n] = fd;
    }
    const char *ready = getenv("HMOS_LOG_READY_PARAM");
    if (ready) {
        if (strncmp(ready, "aurora.boot.", 12)) return 2;
        void *lib = dlopen("/system/lib64/chipset-sdk-sp/libbegetutil.z.so", RTLD_NOW | RTLD_LOCAL);
        int (*publish)(const char *, const char *) = lib ? dlsym(lib, "SystemSetParameter") : NULL;
        if (!publish || publish(ready, "true")) {
            fprintf(stderr, "HILOG_READY_PUBLISH_FAILED\n"); return 2;
        }
        fprintf(stderr, "HILOG_SOCKET_READY %s=true\n", ready);
    }
    if (argc > 1 && strcmp(argv[1], "dump") == 0)
        return dump_loop(fds[0]);
    execl("/system/bin/hilogd", "hilogd", (char *)NULL);
    perror("exec hilogd"); return 2;
}
