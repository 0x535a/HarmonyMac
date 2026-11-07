/* Original RS client IPC: validate the default screen and power query. */
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
int main()
{
    setbuf(stdout, nullptr); alarm(12);
    void *lib=dlopen("/system/lib64/librender_service_client.z.so",RTLD_NOW|RTLD_LOCAL);
    if (!lib) { puts(dlerror()); return 2; }
    auto instance=reinterpret_cast<void *(*)()>(dlsym(lib,"_ZN4OHOS5Rosen12RSInterfaces11GetInstanceEv"));
    auto screen=reinterpret_cast<uint64_t (*)(void *)>(dlsym(lib,"_ZN4OHOS5Rosen12RSInterfaces18GetDefaultScreenIdEv"));
    auto power=reinterpret_cast<uint32_t (*)(void *,uint64_t)>(
        dlsym(lib,"_ZN4OHOS5Rosen12RSInterfaces20GetScreenPowerStatusEm"));
    if (!instance || !screen || !power) { puts("RS_CLIENT_MISSING_SYMBOL"); return 2; }
    void *api=instance();
    uint64_t id=screen(api); uint32_t state=power(api,id);
    printf("RS_CLIENT_SCREEN id=%llu power=%u\n",
           static_cast<unsigned long long>(id),state);
    if (id==UINT64_MAX) return 2;
    puts("RS_CLIENT_SCREEN_PASS");
    return 0;
}
