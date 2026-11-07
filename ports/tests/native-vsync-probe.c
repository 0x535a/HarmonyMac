/* Exercise the original NativeVSync request and real frame callback chain. */
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_int frames;
static void callback(long long timestamp, void *data)
{
    (void)data;
    printf("NATIVE_VSYNC_CALLBACK timestamp=%lld\n", timestamp);
    atomic_fetch_add(&frames, 1);
}

int main(void)
{
    setbuf(stdout, NULL);
    alarm(15);
    void *lib = dlopen("/system/lib64/libnative_vsync.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts(dlerror()); return 2; }
    void *(*create)(const char *, unsigned) = dlsym(lib, "OH_NativeVSync_Create");
    int (*request)(void *, void (*)(long long, void *), void *) = dlsym(lib, "OH_NativeVSync_RequestFrame");
    int (*period)(void *, long long *) = dlsym(lib, "OH_NativeVSync_GetPeriod");
    void (*destroy)(void *) = dlsym(lib, "OH_NativeVSync_Destroy");
    if (!create || !request || !period || !destroy) return 2;
    void *vsync = create("aurora-native-vsync", 19);
    printf("NATIVE_VSYNC_CREATED=%d\n", vsync != NULL);
    if (!vsync) return 2;
    int passed = 1;
    for (int n = 0; n < 3; ++n) {
        int before = atomic_load(&frames);
        int ret = request(vsync, callback, NULL);
        printf("NATIVE_VSYNC_REQUEST n=%d result=%d\n", n, ret);
        for (int i = 0; !ret && i < 40 && atomic_load(&frames) == before; ++i) usleep(50000);
        if (ret || atomic_load(&frames) == before) { passed = 0; break; }
    }
    long long ns = 0;
    int ret = period(vsync, &ns);
    printf("NATIVE_VSYNC_PERIOD result=%d ns=%lld callbacks=%d\n", ret, ns, atomic_load(&frames));
    passed &= !ret && ns > 0 && atomic_load(&frames) >= 3;
    destroy(vsync);
    if (passed) puts("NATIVE_VSYNC_PASS");
    return passed ? 0 : 2;
}
