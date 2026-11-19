/* SPDX-License-Identifier: Apache-2.0
 * FileOperator for the official portable UserAuth implementation. Preserve
 * authentication algorithms and results; fail closed on unreadable state and
 * publish complete credential updates using fsync + atomic rename.
 */
#include "file_operator.h"
#include "defines.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef AURORA_USERAUTH_STORAGE_BASE
#define AURORA_USERAUTH_STORAGE_BASE "/data/service/el1/public/userauth"
#endif
#define STATE_LIMIT (16U * 1024U * 1024U)

static const char *state_name(const char *path)
{
    if (!path) return NULL;
    if (!strcmp(path, "/data/service/el1/public/userauth/userinfo")) return "userinfo";
    if (!strcmp(path, "/data/service/el1/public/userauth/globalConfigInfo")) return "globalConfigInfo";
    return NULL;
}

static int state_dir(void)
{
    int fd = open(AURORA_USERAUTH_STORAGE_BASE, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

static int state_read(const char *path)
{
    const char *name = state_name(path);
    if (!name) { errno = EINVAL; return -1; }
    int dir = state_dir();
    if (dir < 0) {
        // A missing provisioning directory is not an empty credential file.
        if (errno == ENOENT) errno = EACCES;
        return -1;
    }
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int error = errno;
    close(dir);
    if (fd < 0) { errno = error; return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0077) || st.st_size <= 0 || st.st_size > STATE_LIMIT) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

static bool state_exists(const char *path)
{
    int fd = state_read(path);
    if (fd >= 0) { close(fd); return true; }
    // User state is explicitly provisioned once. Losing that file must never
    // recreate a credential-free account after a later password enrollment.
    if (state_name(path) && !strcmp(state_name(path), "userinfo")) return true;
    // The upstream bool API distinguishes only missing/existing. Treat any
    // other error as existing, so its subsequent read fails instead of
    // silently creating an empty credential database.
    return errno != ENOENT;
}

static int32_t state_length(const char *path, uint32_t *length)
{
    if (!length) return RESULT_BAD_PARAM;
    *length = 0;
    int fd = state_read(path);
    if (fd < 0) return RESULT_BAD_READ;
    struct stat st;
    int result = fstat(fd, &st);
    close(fd);
    if (result) return RESULT_BAD_READ;
    *length = (uint32_t)st.st_size;
    return RESULT_SUCCESS;
}

static int32_t state_load(const char *path, uint8_t *buffer, uint32_t length)
{
    if (!buffer || !length || length > STATE_LIMIT) return RESULT_BAD_PARAM;
    int fd = state_read(path);
    if (fd < 0) return RESULT_BAD_READ;
    uint32_t offset = 0;
    while (offset < length) {
        ssize_t n = read(fd, buffer + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); memset(buffer, 0, length); return RESULT_BAD_READ; }
        offset += (uint32_t)n;
    }
    uint8_t extra;
    ssize_t tail;
    do { tail = read(fd, &extra, 1); } while (tail < 0 && errno == EINTR);
    close(fd);
    if (tail != 0) { memset(buffer, 0, length); return RESULT_BAD_READ; }
    return RESULT_SUCCESS;
}

static int32_t state_save(const char *path, const uint8_t *buffer, uint32_t length)
{
    const char *name = state_name(path);
    if (!name || !buffer || !length || length > STATE_LIMIT) return RESULT_BAD_PARAM;
    int dir = state_dir();
    if (dir < 0) return RESULT_BAD_WRITE;
    struct stat st;
    if (!fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
            close(dir); return RESULT_BAD_WRITE;
        }
    } else if (errno != ENOENT) { close(dir); return RESULT_BAD_WRITE; }
    char pending[80];
    snprintf(pending, sizeof(pending), ".%s.%ld.pending", name, (long)getpid());
    int fd = openat(dir, pending, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { close(dir); return RESULT_BAD_WRITE; }
    uint32_t offset = 0;
    bool ok = true;
    while (offset < length) {
        ssize_t n = write(fd, buffer + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += (uint32_t)n;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && renameat(dir, pending, dir, name)) ok = false;
    if (ok && fsync(dir)) ok = false;
    if (!ok) unlinkat(dir, pending, 0);
    close(dir);
    return ok ? RESULT_SUCCESS : RESULT_BAD_WRITE;
}

static int32_t state_delete(const char *path)
{
    const char *name = state_name(path);
    if (!name) return RESULT_BAD_PARAM;
    int fd = state_read(path);
    if (fd < 0) return RESULT_GENERAL_ERROR;
    close(fd);
    int dir = state_dir();
    if (dir < 0) return RESULT_GENERAL_ERROR;
    int result = unlinkat(dir, name, 0);
    if (!result) result = fsync(dir);
    close(dir);
    return result ? RESULT_GENERAL_ERROR : RESULT_SUCCESS;
}

FileOperator *GetDefaultFileOperator(void)
{
    static FileOperator operations = {
        .isFileExist = state_exists, .getFileLen = state_length,
        .readFile = state_load, .writeFile = state_save, .deleteFile = state_delete,
    };
    return &operations;
}
