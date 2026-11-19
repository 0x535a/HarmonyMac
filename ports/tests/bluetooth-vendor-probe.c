/* Real HCI command replies through the vendor H4 stream; software controller
 * tests are explicit and do not qualify physical Bluetooth functionality. */
#define _GNU_SOURCE
#include "ohos_bt_vendor_abi.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static unsigned initialized,failed;
static void init_done(BtOpResultT result){if(result==BTC_OP_RESULT_SUCCESS)++initialized;else ++failed;}
static int fd_count(void)
{
    DIR *d=opendir("/proc/self/fd");if(!d)return -1;
    int n=0;struct dirent *e;while((e=readdir(d)))if(e->d_name[0]!='.')++n;
    closedir(d);return n;
}
static int read_bytes(int fd,uint8_t *buffer,size_t size)
{
    size_t offset=0;
    while(offset<size) {
        struct pollfd p={.fd=fd,.events=POLLIN};int ready=poll(&p,1,3000);
        if(ready<=0 || !(p.revents&POLLIN))return -1;
        ssize_t n=read(fd,buffer+offset,size-offset);if(n<=0)return -1;offset+=(size_t)n;
    }
    return 0;
}
static int event(int fd,unsigned opcode,size_t expected_length)
{
    for(unsigned i=0;i<20;++i) {
        uint8_t packet[258];if(read_bytes(fd,packet,3))return -1;
        if(packet[0]!=4 || read_bytes(fd,packet+3,packet[2]))return -1;
        printf("BLUETOOTH_HCI_EVENT hex=");for(unsigned j=0;j<3u+packet[2];++j)printf("%02x",packet[j]);puts("");
        if(packet[1]==0x0e && packet[2]>=4 && (packet[4]|((unsigned)packet[5]<<8))==opcode) {
            if(packet[6] || packet[2]!=expected_length)return -1;
            if(opcode==0x2003) {
                unsigned bits=0;for(unsigned j=7;j<15;++j)bits|=packet[j];if(!bits)return -1;
            }
            return 0;
        }
    }
    return -1;
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);alarm(45);
    int positive=argc==2 && !strcmp(argv[1],"--virtual-controller");
    int negative=argc==2 && !strcmp(argv[1],"--expect-missing");
    if(!positive && !negative)return 2;
    char cmdline[4096]={0};FILE *f=fopen("/proc/cmdline","r");if(!f)return 2;
    size_t n=fread(cmdline,1,sizeof(cmdline)-1,f);fclose(f);
    if(!n || !strstr(cmdline,"aurora.virtual_identity=1"))return 2;
    void *lib=dlopen("/system/bin/aurora/bluetooth/libbt_vendor.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("BLUETOOTH_VENDOR_DLOPEN_FAIL %s\n",dlerror());return 2;}
    const BtVendorInterfaceT *api=dlsym(lib,"BLUETOOTH_VENDOR_LIB_INTERFACE");
    if(!api || api->size!=sizeof(*api))return 2;
    BtVendorCallbacksT callbacks={.size=sizeof(callbacks),.initCb=init_done};
    int baseline=fd_count(),result=2;
    for(unsigned cycle=0;cycle<3;++cycle) {
        if(api->init(&callbacks,NULL))goto done;
        int rc=api->op(BT_OP_POWER_ON,NULL);printf("BLUETOOTH_VENDOR_POWER cycle=%u result=%d errno=%d\n",cycle,rc,errno);
        if(negative) {
            if(!rc || errno!=ENODEV || initialized || failed)goto done;
        } else {
            if(rc)goto done;
            int channels[HCI_MAX_CHANNEL]={-1,-1,-1,-1};
            if(api->op(BT_OP_HCI_CHANNEL_OPEN,channels)!=1)goto done;
            if(api->op(BT_OP_INIT,NULL) || initialized!=cycle+1 || failed)goto done;
            // The original H4 implementation splits the type and body writes.
            const uint8_t reset[]={1,3,0x0c,0};
            if(write(channels[0],reset,1)!=1 || write(channels[0],reset+1,2)!=2 || write(channels[0],reset+3,1)!=1 || event(channels[0],0x0c03,4))goto done;
            const uint8_t batch[]={1,1,0x10,0,1,9,0x10,0,1,3,0x20,0};
            if(write(channels[0],batch,sizeof(batch))!=(ssize_t)sizeof(batch) || event(channels[0],0x1001,12) ||
                event(channels[0],0x1009,10) || event(channels[0],0x2003,12))goto done;
            if(api->op(BT_OP_LPM_ENABLE,NULL)!=-1)goto done;
        }
        if(api->op(BT_OP_HCI_CHANNEL_CLOSE,NULL) || api->op(BT_OP_POWER_OFF,NULL))goto done;
        api->close();int count=fd_count();printf("BLUETOOTH_VENDOR_FD_COUNT cycle=%u baseline=%d current=%d\n",cycle,baseline,count);
        if(count!=baseline)goto done;
    }
    puts(positive?"BLUETOOTH_VENDOR_VIRTUAL_HCI_PASS cycles=3 commands=12 fabricated_replies=0 physical_rf_verified=0":
        "BLUETOOTH_VENDOR_MISSING_CONTROLLER_PASS cycles=3 callbacks_success=0");result=0;
done:
    api->close();dlclose(lib);printf("BLUETOOTH_VENDOR_PROBE_EXIT=%d\n",result);return result;
}
