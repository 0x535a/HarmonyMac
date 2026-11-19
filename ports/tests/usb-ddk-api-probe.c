/* Public original USB DDK C API. No replacement service or raw USB opens. */
#include <usb/usb_ddk_api.h>
#include <dirent.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(struct UsbDeviceDescriptor) == 24 &&
    offsetof(struct UsbDeviceDescriptor,bNumConfigurations) == 17,
    "NDK descriptor is aligned to 8; its wire payload is 18 bytes");

static unsigned fd_count(void)
{
    DIR *d=opendir("/proc/self/fd");if(!d)return 0;
    unsigned count=0;struct dirent *e;
    while((e=readdir(d)))if(e->d_name[0]!='.')++count;
    closedir(d);return count;
}

static int descriptor_matches_kernel(const unsigned char *actual)
{
    DIR *d=opendir("/sys/bus/usb/devices");if(!d)return 0;
    struct dirent *e;int found=0;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'||strchr(e->d_name,':'))continue;
        char path[512];snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s/descriptors",e->d_name);
        FILE *f=fopen(path,"rb");if(!f)continue;
        unsigned char expected[18];size_t n=fread(expected,1,sizeof(expected),f);fclose(f);
        if(n==18&&!memcmp(actual,expected,18)){found=1;break;}
    }
    closedir(d);return found;
}

int main(void)
{
    setbuf(stdout,NULL);alarm(45);
    printf("USB_DDK_API_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    FILE *status=fopen("/proc/self/status","r");
    if(status){char line[256];while(fgets(line,sizeof(line),status))if(!strncmp(line,"CapEff:",7))fputs(line,stdout);fclose(status);}
    void *lib=dlopen("/system/lib64/ndk/libusb_ndk.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("USB_DDK_DLOPEN_FAILED %s\n",dlerror());return 2;}
    int32_t (*init)(void)=dlsym(lib,"OH_Usb_Init");
    int32_t (*get_devices)(struct Usb_DeviceArray *)=dlsym(lib,"OH_Usb_GetDevices");
    int32_t (*get_descriptor)(uint64_t,struct UsbDeviceDescriptor *)=dlsym(lib,"OH_Usb_GetDeviceDescriptor");
    int32_t (*release)(void)=dlsym(lib,"OH_Usb_ReleaseResource");
    if(!init||!get_devices||!get_descriptor||!release){puts("USB_DDK_PUBLIC_API_UNAVAILABLE");return 2;}
    // This product API writes into a caller-owned buffer and has no capacity
    // argument. Cover the entire Linux USB bus/address space, with headroom;
    // the controlled VM has no physical passthrough and at most a few devices.
    enum { DEVICE_ID_CAPACITY=65536 };
    uint64_t *ids=calloc(DEVICE_ID_CAPACITY,sizeof(*ids));if(!ids)return 2;
    unsigned before=fd_count();int failed=0;
    for(unsigned round=0;round<3;++round){
        int32_t result=init();printf("USB_DDK_INIT round=%u result=%d\n",round,result);
        if(result){failed=1;break;}
        struct Usb_DeviceArray devices={.deviceIds=ids,.num=0};
        result=get_devices(&devices);
        printf("USB_DDK_DEVICES round=%u result=%d count=%u\n",round,result,devices.num);
        if(result||devices.num>DEVICE_ID_CAPACITY)failed=1;
        else for(uint32_t i=0;i<devices.num;++i){
            struct UsbDeviceDescriptor descriptor={0};
            int32_t read=get_descriptor(ids[i],&descriptor);
            const unsigned char *bytes=(const unsigned char *)&descriptor;
            int match=!read&&descriptor_matches_kernel(bytes);
            printf("USB_DDK_DESCRIPTOR id=%" PRIu64 " result=%d kernel_match=%d vendor=%04x product=%04x\n",
                ids[i],read,match,bytes[8]|(bytes[9]<<8),bytes[10]|(bytes[11]<<8));
            if(!match)failed=1;
        }
        int32_t end=release();
        printf("USB_DDK_RELEASE round=%u result=%d fds=%u\n",round,end,fd_count());
        if(end)failed=1;
        if(failed)break;
    }
    printf("USB_DDK_RESULT failed=%d fds_before=%u fds_after=%u physical_passthrough=0\n",failed,before,fd_count());
    free(ids);
    return failed?2:0;
}
