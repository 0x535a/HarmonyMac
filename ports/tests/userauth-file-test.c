#include "file_operator.h"
#include "defines.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/data/boot/aurora/userauth-fixture-data/io"
#define USER_PATH "/data/service/el1/public/userauth/userinfo"

int main(void)
{
    assert(geteuid() == 2000);
    assert(mkdir(ROOT, 0700) == 0 || errno == EEXIST);
    FileOperator *f = GetDefaultFileOperator();
    uint32_t length = 0;
    // Missing required state fails closed; optional config may be absent.
    assert(f->isFileExist(USER_PATH));
    assert(f->getFileLen(USER_PATH, &length) != RESULT_SUCCESS);
    assert(!f->isFileExist("/data/service/el1/public/userauth/globalConfigInfo"));
    const uint8_t first[] = "persisted credential metadata";
    const uint8_t second[] = "replacement credential metadata";
    uint8_t data[sizeof(second)] = {0};
    assert(f->writeFile(USER_PATH, first, sizeof(first)) == RESULT_SUCCESS);
    assert(f->getFileLen(USER_PATH, &length) == RESULT_SUCCESS && length == sizeof(first));
    assert(f->readFile(USER_PATH, data, length) == RESULT_SUCCESS && !memcmp(data, first, length));
    struct stat st;
    assert(stat(ROOT "/userinfo", &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == geteuid());

    // A failed replacement must leave the last durable record intact.
    char pending[160];
    snprintf(pending, sizeof(pending), ROOT "/.userinfo.%ld.pending", (long)getpid());
    int fd = open(pending, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0); close(fd);
    assert(f->writeFile(USER_PATH, second, sizeof(second)) != RESULT_SUCCESS);
    assert(f->getFileLen(USER_PATH, &length) == RESULT_SUCCESS && length == sizeof(first));
    assert(f->readFile(USER_PATH, data, length) == RESULT_SUCCESS && !memcmp(data, first, length));
    assert(unlink(pending) == 0);
    assert(f->writeFile(USER_PATH, second, sizeof(second)) == RESULT_SUCCESS);
    assert(f->readFile(USER_PATH, data, sizeof(second)) == RESULT_SUCCESS && !memcmp(data, second, sizeof(second)));

    assert(chmod(ROOT "/userinfo", 0644) == 0);
    assert(f->isFileExist(USER_PATH) && f->getFileLen(USER_PATH, &length) != RESULT_SUCCESS);
    assert(f->writeFile(USER_PATH, first, sizeof(first)) != RESULT_SUCCESS);
    assert(chmod(ROOT "/userinfo", 0600) == 0);
    assert(truncate(ROOT "/userinfo", 0) == 0);
    assert(f->isFileExist(USER_PATH) && f->getFileLen(USER_PATH, &length) != RESULT_SUCCESS);
    assert(unlink(ROOT "/userinfo") == 0);

    fd = open(ROOT "/symlink-target", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, first, sizeof(first)) == sizeof(first)); close(fd);
    assert(symlink(ROOT "/symlink-target", ROOT "/userinfo") == 0);
    assert(f->isFileExist(USER_PATH) && f->getFileLen(USER_PATH, &length) != RESULT_SUCCESS);
    assert(f->writeFile(USER_PATH, second, sizeof(second)) != RESULT_SUCCESS);
    fd = open(ROOT "/symlink-target", O_RDONLY);
    assert(fd >= 0 && read(fd, data, sizeof(first)) == sizeof(first) && !memcmp(data, first, sizeof(first))); close(fd);
    assert(unlink(ROOT "/userinfo") == 0 && unlink(ROOT "/symlink-target") == 0);
    assert(rmdir(ROOT) == 0);
    assert(f->isFileExist(USER_PATH) && f->getFileLen(USER_PATH, &length) != RESULT_SUCCESS);
    puts("USERAUTH_FILE_ATOMICITY_PERMISSIONS_AND_FAIL_CLOSED_PASS");
}
