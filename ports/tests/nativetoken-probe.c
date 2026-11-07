/* Diagnostic: exercise the original native-token flow end to end.
 * Calls the prebuilt libnativetoken_shared.z.so GetAccessTokenId() exactly as
 * init does, then sets the returned id through the accesstokenid driver and
 * verifies fork inheritance. Pinpoints where the original init flow stops. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct TokenInfoParams {
    int32_t dcapsNum;
    int32_t permsNum;
    int32_t aclsNum;
    const char **dcaps;
    const char **perms;
    const char **acls;
    const char *processName;
    const char *aplStr;
} NativeTokenInfoParams;

extern uint64_t GetAccessTokenId(NativeTokenInfoParams *tokenInfo);

#define ACCESS_TOKENID_GET_TOKENID _IOR('A', 1, unsigned long long)
#define ACCESS_TOKENID_SET_TOKENID _IOW('A', 2, unsigned long long)
#define DIR_PATH "/data/service/el0/access_token"
#define JSON_PATH DIR_PATH "/nativetoken.json"

static uint64_t get_self_token(int fd)
{
    uint64_t value = 0;
    if (ioctl(fd, ACCESS_TOKENID_GET_TOKENID, &value) != 0) {
        printf("NATIVETOKEN_PROBE get_token_failed errno=%d (%s)\n", errno, strerror(errno));
    }
    return value;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    const char *name = argc > 1 ? argv[1] : "aurora-diag";
    struct stat st;
    printf("NATIVETOKEN_PROBE begin name=%s pid=%d\n", name, getpid());
    printf("NATIVETOKEN_PROBE stat_dir ret=%d errno=%d mode=0%o\n",
           stat(DIR_PATH, &st), errno, stat(DIR_PATH, &st) == 0 ? (unsigned)st.st_mode : 0);
    printf("NATIVETOKEN_PROBE stat_json ret=%d errno=%d\n", stat(JSON_PATH, &st), errno);

    NativeTokenInfoParams params = {0, 0, 0, NULL, NULL, NULL, name, "system_basic"};
    uint64_t id = GetAccessTokenId(&params);
    printf("NATIVETOKEN_PROBE GetAccessTokenId id=0x%llx type=%u version=%u unique=%u\n",
           (unsigned long long)id, (unsigned)((id >> 27) & 3),
           (unsigned)((id >> 29) & 7), (unsigned)(id & 0xfffff));
    printf("NATIVETOKEN_PROBE stat_json_after ret=%d errno=%d\n", stat(JSON_PATH, &st), errno);
    if (id == 0) {
        printf("NATIVETOKEN_PROBE result=fail_get_id\n");
        return 1;
    }

    int fd = open("/dev/access_token_id", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("NATIVETOKEN_PROBE open_dev_failed errno=%d (%s)\n", errno, strerror(errno));
        return 2;
    }
    uint64_t before = get_self_token(fd);
    int setret = ioctl(fd, ACCESS_TOKENID_SET_TOKENID, &id);
    printf("NATIVETOKEN_PROBE set_token ret=%d errno=%d before=0x%llx after=0x%llx\n",
           setret, setret < 0 ? errno : 0, (unsigned long long)before,
           (unsigned long long)get_self_token(fd));
    if (setret != 0) {
        return 3;
    }
    pid_t child = fork();
    if (child == 0) {
        int cfd = open("/dev/access_token_id", O_RDWR | O_CLOEXEC);
        uint64_t inherited = cfd >= 0 ? get_self_token(cfd) : 0;
        printf("NATIVETOKEN_PROBE child pid=%d inherited=0x%llx match=%d\n",
               getpid(), (unsigned long long)inherited, inherited == id);
        _exit(inherited == id ? 0 : 4);
    }
    int status = 0;
    waitpid(child, &status, 0);
    printf("NATIVETOKEN_PROBE child_exit=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    printf("NATIVETOKEN_PROBE result=%s\n",
           WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "pass" : "fail_inherit");
    close(fd);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 4;
}
