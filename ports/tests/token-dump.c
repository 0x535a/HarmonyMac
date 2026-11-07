/* Diagnostic: dump the calling process's OHOS AccessToken identity.
 * Reads current->token / current->ftoken via the accesstokenid driver and
 * decodes the 32-bit AccessTokenIDInner type/version fields (type at bits
 * 27..28, TOKEN_NATIVE == 1). Also optionally reports a target pid's token by
 * re-execing under the caller. Runs as an OHOS-ABI process inside the target
 * root so it reflects the real inherited identity. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ACCESS_TOKEN_ID_IOCTL_BASE 'A'
#define GET_TOKEN_ID 1
#define GET_FTOKEN_ID 3
#define ACCESS_TOKENID_GET_TOKENID \
    _IOR(ACCESS_TOKEN_ID_IOCTL_BASE, GET_TOKEN_ID, unsigned long long)
#define ACCESS_TOKENID_GET_FTOKENID \
    _IOR(ACCESS_TOKEN_ID_IOCTL_BASE, GET_FTOKEN_ID, unsigned long long)

static const char *type_name(unsigned t)
{
    switch (t) {
    case 0: return "HAP";
    case 1: return "NATIVE";
    case 2: return "SHELL";
    default: return "BUTT";
    }
}

int main(int argc, char **argv)
{
    const char *label = argc > 1 ? argv[1] : "self";
    int fd = open("/dev/access_token_id", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("TOKEN_DUMP label=%s pid=%d open_failed errno=%d (%s)\n",
               label, getpid(), errno, strerror(errno));
        return 1;
    }
    unsigned long long token = 0, ftoken = 0;
    int rt = ioctl(fd, ACCESS_TOKENID_GET_TOKENID, &token);
    int et = rt < 0 ? errno : 0;
    int rf = ioctl(fd, ACCESS_TOKENID_GET_FTOKENID, &ftoken);
    int ef = rf < 0 ? errno : 0;
    unsigned id32 = (unsigned)(token & 0xffffffffULL);
    unsigned type = (id32 >> 27) & 0x3;
    unsigned version = (id32 >> 29) & 0x7;
    unsigned unique = id32 & 0xfffff;
    printf("TOKEN_DUMP label=%s pid=%d get_token_ret=%d errno=%d token=0x%llx "
           "id32=0x%08x unique=%u version=%u type=%u(%s)\n",
           label, getpid(), rt, et, token, id32, unique, version, type,
           rt == 0 ? type_name(type) : "n/a");
    printf("TOKEN_DUMP label=%s pid=%d get_ftoken_ret=%d errno=%d ftoken=0x%llx\n",
           label, getpid(), rf, ef, ftoken);
    close(fd);
    return 0;
}
