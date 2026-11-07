/* Query the unchanged HDI C service manager; optionally publish observed readiness. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Public HDIServiceManager C ABI from OH6 servmgr_hdi.h. Opaque objects are
 * allocated/released by the original platform library, never by this probe. */
struct HdfRemoteService;
struct ServiceStatusListener;
struct HdiServiceSet;
struct HDIServiceManager {
    struct HdfRemoteService *(*GetService)(struct HDIServiceManager *, const char *);
    int32_t (*RegisterServiceStatusListener)(struct HDIServiceManager *, struct ServiceStatusListener *, uint16_t);
    int32_t (*UnregisterServiceStatusListener)(struct HDIServiceManager *, struct ServiceStatusListener *);
    struct HdiServiceSet *(*ListServiceByInterfaceDesc)(struct HDIServiceManager *, const char *);
};
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    unsigned seconds = 0;
    int manager_only = 0;
    const char *publish = NULL;
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "--wait") && first + 1 < argc) {
            char *end; unsigned long n = strtoul(argv[first+1], &end, 10);
            if (*end || n > 120) return 2;
            seconds = n; first += 2;
        } else if (!strcmp(argv[first], "--manager-only")) {
            manager_only = 1; first++;
        } else if (!strcmp(argv[first], "--publish") && first + 1 < argc) {
            publish = argv[first+1]; first += 2;
        } else return 2;
    }
    if ((first == argc && !manager_only) || (publish && strncmp(publish, "aurora.boot.", 12))) return 2;
    alarm(seconds + 15);
    void *lib = dlopen("/system/lib64/chipset-sdk-sp/libhdi.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts(dlerror()); return 2; }
    struct HDIServiceManager *(*get)(void) = dlsym(lib, "HDIServiceManagerGet");
    void (*release)(struct HDIServiceManager *) = dlsym(lib, "HDIServiceManagerRelease");
    void (*recycle)(struct HdfRemoteService *) = dlsym(lib, "HdfRemoteServiceRecycle");
    if (!get || !release || !recycle) return 2;
    double deadline = now() + seconds;
    for (;;) {
        int ready = 1;
        struct HDIServiceManager *manager = get();
        if (!manager) ready = 0;
        else {
            for (int i = first; i < argc; ++i) {
                struct HdfRemoteService *service = manager->GetService(manager, argv[i]);
                if (service) recycle(service);
                else ready = 0;
            }
            release(manager);
        }
        if (ready) {
            printf("HDI_READY_OBSERVED kind=%s monotonic=%.6f\n", manager_only ? "manager" : "services", now());
            if (manager_only) puts("HDI_MANAGER_READY");
            for (int i = first; i < argc; ++i) printf("HDI_SERVICE_READY name=%s\n", argv[i]);
            if (publish) {
                void *params = dlopen("/system/lib64/chipset-sdk-sp/libbegetutil.z.so", RTLD_NOW | RTLD_LOCAL);
                int (*set)(const char *, const char *) = params ? dlsym(params, "SystemSetParameter") : NULL;
                if (!set || set(publish, "true")) return 2;
                printf("HDI_READY_PUBLISHED %s=true\n", publish);
            }
            puts("HDI_SERVICE_READY_PASS"); return 0;
        }
        if (now() >= deadline) { puts("HDI_SERVICE_READY_TIMEOUT"); return 2; }
        usleep(200000);
    }
}
