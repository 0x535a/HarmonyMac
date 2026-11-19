/* SPDX-License-Identifier: Apache-2.0
 * Give stock recursive sandbox binds a dedicated shared external-volume root.
 * A self bind preserves existing files and DAC; it never mounts a data device.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int prepare(const char *path)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
        fprintf(stderr, "AURORA_STORAGE_ROOT_REJECTED path=%s\n", path);
        return 1;
    }
    FILE *mounts = fopen("/proc/self/mountinfo", "re");
    if (!mounts) return 1;
    char line[8192], target[4096], root[4096];
    int mounted = 0, shared = 0;
    while (fgets(line, sizeof(line), mounts)) {
        if (sscanf(line, "%*u %*u %*s %4095s %4095s", root, target) != 2) continue;
        if (strcmp(target, path)) continue;
        /* Only adopt our self bind, never a filesystem mounted over this path. */
        if (strcmp(root, path)) {
            fclose(mounts);
            fprintf(stderr, "AURORA_STORAGE_ROOT_FOREIGN_MOUNT path=%s\n", path);
            return 1;
        }
        mounted = 1;
        shared = strstr(line, " shared:") != NULL;
    }
    fclose(mounts);
    if (!mounted && mount(path, path, NULL, MS_BIND, NULL)) {
        fprintf(stderr, "AURORA_STORAGE_ROOT_BIND_FAILED path=%s errno=%d\n", path, errno);
        return 1;
    }
    if (!shared && mount(NULL, path, NULL, MS_SHARED, NULL)) {
        int error = errno;
        /* Undo only the new self bind; never detach a pre-existing mount. */
        if (!mounted && umount(path))
            fprintf(stderr, "AURORA_STORAGE_ROOT_BIND_UNDO_FAILED path=%s errno=%d\n", path, errno);
        fprintf(stderr, "AURORA_STORAGE_ROOT_SHARED_FAILED path=%s errno=%d\n", path, error);
        return 1;
    }
    printf("AURORA_STORAGE_SHARED_ROOT_READY path=%s existing=%d\n", path, mounted);
    return 0;
}

int main(void)
{
    if (geteuid() != 0) return 1;
    int result = prepare("/mnt/data/external");
    result |= prepare("/mnt/data/external_fuse");
    return result;
}
