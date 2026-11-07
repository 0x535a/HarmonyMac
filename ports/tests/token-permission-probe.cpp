/* Compare kernel identity with the original AccessToken permission API. */
#include <cstdio>
#include <cstdint>
#include <string>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main()
{
    setbuf(stdout,nullptr);alarm(12);
    unsigned long long id=0;int fd=open("/dev/access_token_id",O_RDWR|O_CLOEXEC);
    if (fd<0 || ioctl(fd,_IOR('A',1,unsigned long long),&id)) return 2;
    close(fd);
    void *lib=dlopen("/system/lib64/platformsdk/libaccesstoken_sdk.z.so",RTLD_NOW|RTLD_LOCAL);
    if (!lib) {puts(dlerror());return 2;}
    auto verify=reinterpret_cast<int (*)(uint32_t,const std::string&)>(dlsym(lib,
        "_ZN4OHOS8Security11AccessToken14AccessTokenKit17VerifyAccessTokenEjRKNSt3__h12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
    auto native=reinterpret_cast<uint32_t (*)(const std::string&)>(dlsym(lib,
        "_ZN4OHOS8Security11AccessToken14AccessTokenKit16GetNativeTokenIdERKNSt3__h12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
    if (!verify||!native) return 2;
    uint32_t lookup=native("aurora-boot-check");
    int result=verify(static_cast<uint32_t>(id),"ohos.permission.DUMP");
    printf("TOKEN_PERMISSION_API token=0x%llx lookup=0x%x dump_result=%d\n",id,lookup,result);
    return result==0 && lookup==static_cast<uint32_t>(id) ? 0 : 2;
}
