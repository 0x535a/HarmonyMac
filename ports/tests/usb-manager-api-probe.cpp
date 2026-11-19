/* Official UsbDevice objects and the unchanged UsbSrvClient IPC API.
 * GetInstance owns the opaque client; no client object layout is reproduced.
 */
#include "usb_device.h"
#include <dirent.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using OHOS::USB::UsbDevice;
static_assert(sizeof(UsbDevice) == 0xd8, "Target vector element stride is 0xd8");

static unsigned fd_count()
{
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return 0;
    unsigned count = 0;
    while (dirent *entry = readdir(dir)) if (entry->d_name[0] != '.') ++count;
    closedir(dir);
    return count;
}

int main(int argc, char **argv)
{
    unsigned seconds = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 35;
    if (seconds < 3 || seconds > 60) return 2;
    setbuf(stdout, nullptr);
    alarm(seconds + 25);
    printf("USB_MANAGER_IDENTITY uid=%u gid=%u\n", getuid(), getgid());
    if (FILE *status = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), status)) if (!strncmp(line, "CapEff:", 7)) fputs(line, stdout);
        fclose(status);
    }
    printf("USB_MANAGER_ABI device=%zu config=%zu interface=%zu endpoint=%zu\n", sizeof(UsbDevice),
        sizeof(OHOS::USB::USBConfig), sizeof(OHOS::USB::UsbInterface), sizeof(OHOS::USB::USBEndpoint));
    void *lib = dlopen("/system/lib64/platformsdk/libusbsrv_client.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("USB_MANAGER_DLOPEN_FAILED %s\n", dlerror()); return 2; }
    auto instance = reinterpret_cast<void *(*)()>(dlsym(lib, "_ZN4OHOS3USB12UsbSrvClient11GetInstanceEv"));
    auto devices = reinterpret_cast<int32_t (*)(void *, std::vector<UsbDevice> &)>(dlsym(lib,
        "_ZN4OHOS3USB12UsbSrvClient10GetDevicesERNSt3__h6vectorINS0_9UsbDeviceENS2_9allocatorIS4_EEEE"));
    if (!instance || !devices) { puts("USB_MANAGER_API_MISSING"); return 2; }
    void *client = instance();
    if (!client) return 2;
    unsigned before = fd_count();
    int failed = 0;
    puts("USB_MANAGER_READY");
    for (unsigned round = 0; round < seconds; ++round) {
        std::vector<UsbDevice> list;
        int result = devices(client, list);
        printf("USB_MANAGER_SAMPLE round=%u result=%d count=%zu fds=%u\n", round, result, list.size(), fd_count());
        if (result || list.size() > 32) { failed = 1; break; }
        for (auto &device : list) {
            printf("USB_MANAGER_DEVICE round=%u name=%s bus=%u address=%u vendor=%04x product=%04x configs=%zu\n",
                round, device.GetName().c_str(), device.GetBusNum(), device.GetDevAddr(),
                device.GetVendorId(), device.GetProductId(), device.GetConfigs().size());
        }
        sleep(1);
    }
    printf("USB_MANAGER_RESULT failed=%d fds_before=%u fds_after=%u\n", failed, before, fd_count());
    return failed ? 2 : 0;
}
