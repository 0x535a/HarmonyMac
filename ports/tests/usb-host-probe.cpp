/* Original USB Host HDI passthrough implementation; no replacement service. */
#include "usb/v2_0/iusb_host_interface.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <set>
#include <unistd.h>

using namespace OHOS;
using namespace OHOS::HDI::Usb::V2_0;
static std::mutex event_lock;
static std::set<std::pair<int,int>> pending;
static std::atomic<unsigned> events;
class Subscriber final : public IUsbdSubscriber {
public:
    int32_t DeviceEvent(const USBDeviceInfo &info) override
    {
        printf("USB_HOST_EVENT status=%d bus=%d address=%d\n",info.status,info.busNum,info.devNum);
        if (info.busNum>0 && info.busNum<256 && info.devNum>0 && info.devNum<128) {
            std::lock_guard<std::mutex> lock(event_lock);
            pending.emplace(info.busNum,info.devNum);
        }
        ++events;
        return 0;
    }
    int32_t PortChangedEvent(const PortInfo &p) override
    {
        printf("USB_HOST_PORT id=%d power=%d data=%d mode=%d supported=%d\n",
               p.portId,p.powerRole,p.dataRole,p.mode,p.supportedModes);
        return 0;
    }
};
static bool sysfs_descriptor(int bus,int address,unsigned char *bytes)
{
    DIR *dir=opendir("/sys/bus/usb/devices");if (!dir)return false;
    struct dirent *entry;bool found=false;
    while ((entry=readdir(dir))) {
        if (entry->d_name[0]=='.' || strchr(entry->d_name,':'))continue;
        char path[512];int b=-1,a=-1;
        snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s/busnum",entry->d_name);
        FILE *f=fopen(path,"r");if (!f)continue;int rb=fscanf(f,"%d",&b);fclose(f);
        snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s/devnum",entry->d_name);
        f=fopen(path,"r");if (!f)continue;int ra=fscanf(f,"%d",&a);fclose(f);
        if (rb!=1 || ra!=1 || b!=bus || a!=address)continue;
        snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s/descriptors",entry->d_name);
        f=fopen(path,"rb");if (!f)break;found=fread(bytes,1,18,f)==18;fclose(f);break;
    }
    closedir(dir);return found;
}
int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);
    unsigned seconds=argc>1?strtoul(argv[1],nullptr,10):20;
    if (seconds<1 || seconds>180 || argc>2)return 2;
    alarm(seconds+20);
    sptr<IUsbHostInterface> host=IUsbHostInterface::Get(true);
    if (!host) { puts("USB_HOST_GET_FAILED");return 2; }
    uint32_t major=0,minor=0;int ret=host->GetVersion(major,minor);
    printf("USB_HOST_VERSION result=%d major=%u minor=%u uid=%u\n",ret,major,minor,getuid());
    if (ret || major!=2 || minor!=0)return 2;
    sptr<IUsbdSubscriber> subscriber=new Subscriber;
    ret=host->BindUsbdHostSubscriber(subscriber);
    printf("USB_HOST_BIND result=%d\n",ret);if (ret)return 2;
    unsigned good=0,bad=0;
    std::set<std::pair<int,int>> checked;
    puts("USB_HOST_PROBE_READY");
    for (unsigned n=0;n<seconds*10;++n) {
        std::set<std::pair<int,int>> current;
        { std::lock_guard<std::mutex> lock(event_lock);current.swap(pending); }
        for (auto device:current) {
            unsigned char actual[18];
            if (!sysfs_descriptor(device.first,device.second,actual) || checked.count(device))continue;
            UsbDev dev={static_cast<uint8_t>(device.first),static_cast<uint8_t>(device.second)};
            int opened=host->OpenDevice(dev);
            std::vector<uint8_t> descriptor;
            int read=opened?opened:host->GetDeviceDescriptor(dev,descriptor);
            int closed=opened?0:host->CloseDevice(dev);
            bool matches=!opened && !read && !closed && descriptor.size()==18 && !memcmp(actual,descriptor.data(),18);
            printf("USB_HOST_DESCRIPTOR bus=%u address=%u open=%d read=%d close=%d bytes=%zu sysfs_match=%d vendor=%04x product=%04x\n",
                   dev.busNum,dev.devAddr,opened,read,closed,descriptor.size(),matches,
                   actual[8]|(actual[9]<<8),actual[10]|(actual[11]<<8));
            if (matches && (actual[8]|(actual[9]<<8))==0x0627)++good;
            if (!matches)++bad;
            checked.insert(device);
        }
        usleep(100000);
    }
    int unbound=host->UnbindUsbdHostSubscriber(subscriber);
    UsbDev absent={255,127};int missing=host->OpenDevice(absent);
    if (!missing)host->CloseDevice(absent);
    printf("USB_HOST_RESULT callbacks=%u qemu_descriptors=%u failures=%u unbind=%d absent_open=%d\n",
           events.load(),good,bad,unbound,missing);
    if (!good || bad || unbound || !missing)return 2;
    puts("USB_HOST_RELEASE_BEGIN");
    subscriber=nullptr;
    puts("USB_HOST_SUBSCRIBER_RELEASED");
    host=nullptr;
    puts("USB_HOST_OBJECT_RELEASED");
    puts("USB_HOST_PASSTHROUGH_OPERATIONS_PASS physical_hardware=0 process_exit_must_be_checked=1");return 0;
}
