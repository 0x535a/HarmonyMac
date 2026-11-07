#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binderfs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    const char *names[] = {"binder", "hwbinder", "vndbinder"};
    int control = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    if (control < 0) {
        perror("binder-control");
        return 1;
    }
    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        struct binderfs_device device = {0};
        char actual[128], alias[128];
        snprintf(actual, sizeof(actual), "/dev/binderfs/%s", names[i]);
        snprintf(alias, sizeof(alias), "/dev/%s", names[i]);
        snprintf(device.name, sizeof(device.name), "%s", names[i]);
        if (access(actual, F_OK) && ioctl(control, BINDER_CTL_ADD, &device)) {
            perror("BINDER_CTL_ADD");
            return 1;
        }
        if (chmod(actual, 0666)) {
            perror("chmod binder device");
            return 1;
        }
        struct stat state;
        if (lstat(alias, &state)) {
            if (errno != ENOENT || symlink(actual, alias)) {
                perror("binder alias");
                return 1;
            }
        } else if (!S_ISLNK(state.st_mode)) {
            fprintf(stderr, "Refusing to replace existing binder node %s\n", alias);
            return 1;
        } else {
            char existing[128];
            ssize_t size = readlink(alias, existing, sizeof(existing) - 1);
            if (size < 0) return 1;
            existing[size] = '\0';
            if (strcmp(existing, actual)) return 1;
        }
    }
    close(control);
    puts("BINDER_DEVICES_READY");
    return 0;
}
