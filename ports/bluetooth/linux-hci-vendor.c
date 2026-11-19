/* OHOS H4 stream transport over exclusive Linux HCI_CHANNEL_USER.
 * Linux owns firmware loading. Commands/events pass byte-for-byte; this library
 * never synthesizes controller replies, features, addresses or pairing results.
 */
#define _GNU_SOURCE
#include "ohos_bt_vendor_abi.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AURORA_AF_BLUETOOTH 31
#define AURORA_BTPROTO_HCI 1
#define AURORA_HCI_CHANNEL_USER 1
#define H4_MAX 65540
struct HciAddress { unsigned short family,device,channel; };
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static const BtVendorCallbacksT *callbacks;
static int hci=-1,stream=-1,client=-1,thread_created;
static pthread_t thread;
static atomic_int stop=1,alive;

/* Zero means incomplete header; SIZE_MAX means invalid H4 framing. */
static size_t packet_size(const uint8_t *p,size_t size,int outbound)
{
    if(!size)return 0;
    size_t header,length;
    switch(p[0]) {
        case 1: if(!outbound)return SIZE_MAX;header=4;if(size<header)return 0;length=p[3];break;
        case 2: header=5;if(size<header)return 0;length=p[3]|((size_t)p[4]<<8);break;
        case 3: header=4;if(size<header)return 0;length=p[3];break;
        case 4: if(outbound)return SIZE_MAX;header=3;if(size<header)return 0;length=p[2];break;
        case 5: header=5;if(size<header)return 0;length=(p[3]|((size_t)p[4]<<8))&0x3fff;break;
        default:return SIZE_MAX;
    }
    return header+length;
}

static int wait_writable(int fd)
{
    for(unsigned i=0;i<20 && !atomic_load(&stop);++i) {
        struct pollfd p={.fd=fd,.events=POLLOUT};int n=poll(&p,1,100);
        if(n>0)return (p.revents&POLLOUT)?0:-1;
        if(n<0 && errno!=EINTR)return -1;
    }
    errno=ETIMEDOUT;return -1;
}

static int forward_bytes(int fd,const uint8_t *data,size_t size,int datagram)
{
    size_t offset=0;
    while(offset<size && !atomic_load(&stop)) {
        ssize_t n=send(fd,data+offset,size-offset,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n<0) {
            if(errno==EINTR)continue;
            if((errno==EAGAIN || errno==EWOULDBLOCK) && !wait_writable(fd))continue;
            return -1;
        }
        if(!n || (datagram && (size_t)n!=size)){errno=EIO;return -1;}
        offset+=(size_t)n;
    }
    return offset==size?0:-1;
}

static void *transport_thread(void *unused)
{
    (void)unused;
    uint8_t pending[H4_MAX],incoming[H4_MAX];size_t buffered=0;
    unsigned commands=0,packets=0;
    while(!atomic_load(&stop)) {
        struct pollfd p[2]={{.fd=stream,.events=POLLIN},{.fd=hci,.events=POLLIN}};
        int ready=poll(p,2,100);
        if(ready<0){if(errno==EINTR)continue;break;}
        if(p[0].revents&POLLIN) {
            ssize_t n=recv(stream,pending+buffered,sizeof(pending)-buffered,MSG_DONTWAIT);
            if(n<0){if(errno!=EAGAIN && errno!=EINTR)break;}
            else {
                if (!n) break;
                buffered += (size_t)n;
                size_t consumed=0;
                while(consumed<buffered) {
                    size_t length=packet_size(pending+consumed,buffered-consumed,1);
                    if(length==SIZE_MAX || length>H4_MAX)goto done;
                    if(!length || length>buffered-consumed)break;
                    if(forward_bytes(hci,pending+consumed,length,1))goto done;
                    if(pending[consumed]==1)++commands;
                    consumed+=length;
                }
                memmove(pending,pending+consumed,buffered-consumed);buffered-=consumed;
                if(buffered==sizeof(pending))break;
            }
        }
        if(p[1].revents&POLLIN) {
            struct iovec iov={.iov_base=incoming,.iov_len=sizeof(incoming)};
            struct msghdr message={.msg_iov=&iov,.msg_iovlen=1};
            ssize_t n=recvmsg(hci,&message,MSG_DONTWAIT);
            if(n<0){if(errno!=EAGAIN && errno!=EINTR)break;}
            else {
                if(!n || (message.msg_flags&MSG_TRUNC) || packet_size(incoming,(size_t)n,0)!=(size_t)n)break;
                if(forward_bytes(stream,incoming,(size_t)n,0))break;
                ++packets;
            }
        }
        if((p[0].revents|p[1].revents)&(POLLERR|POLLHUP|POLLNVAL))break;
    }
done:
    atomic_store(&alive,0);shutdown(stream,SHUT_RDWR);
    fprintf(stderr,"AURORA_HCI_TRANSPORT_STOP commands=%u received_packets=%u partial_bytes=%zu\n",commands,packets,buffered);
    return NULL;
}

/* Require a single enumerated controller; never steal an active kernel/BlueZ
 * controller using HCIDEVDOWN or silently choose one of multiple devices. */
static int open_controller(void)
{
    DIR *dir=opendir("/sys/class/bluetooth");if(!dir){errno=ENODEV;return -1;}
    unsigned count=0,index=0;struct dirent *entry;
    while((entry=readdir(dir))) {
        if(strncmp(entry->d_name,"hci",3) || !entry->d_name[3])continue;
        char *end;unsigned long value=strtoul(entry->d_name+3,&end,10);
        if(*end || value>=65535)continue;
        ++count;index=(unsigned)value;
    }
    closedir(dir);
    if(count!=1){errno=count?ENOTUNIQ:ENODEV;return -1;}
    int fd=socket(AURORA_AF_BLUETOOTH,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,AURORA_BTPROTO_HCI);
    if(fd<0)return -1;
    struct HciAddress address={AURORA_AF_BLUETOOTH,(unsigned short)index,AURORA_HCI_CHANNEL_USER};
    if(bind(fd,(struct sockaddr *)&address,sizeof(address))) {
        int error=errno;close(fd);errno=error;return -1;
    }
    fprintf(stderr,"AURORA_HCI_USER_BOUND controller=hci%u channel=1\n",index);return fd;
}

static void close_transport(void)
{
    atomic_store(&stop,1);
    if(stream>=0)shutdown(stream,SHUT_RDWR);
    if(client>=0)shutdown(client,SHUT_RDWR);
    if(thread_created){pthread_join(thread,NULL);thread_created=0;}
    if(stream>=0)close(stream);
    if(client>=0)close(client);
    if(hci>=0)close(hci);
    stream=client=hci=-1;atomic_store(&alive,0);
}

static int vendor_init(const BtVendorCallbacksT *cb,unsigned char *address)
{
    (void)address; // The controller's real BD_ADDR is read by the original stack.
    if(!cb || cb->size<sizeof(*cb) || !cb->initCb){errno=EINVAL;return -1;}
    pthread_mutex_lock(&lock);close_transport();callbacks=cb;pthread_mutex_unlock(&lock);return 0;
}

static int vendor_op(BtOpcodeT op,void *parameter)
{
    int result=-1;void (*notify)(BtOpResultT)=NULL;BtOpResultT status=BTC_OP_RESULT_FAIL;
    pthread_mutex_lock(&lock);
    switch(op) {
        case BT_OP_POWER_ON:
            if(!callbacks){errno=EINVAL;break;}
            if(hci<0)hci=open_controller();
            result=hci>=0?0:-1;
            if(result)fprintf(stderr,"AURORA_HCI_UNAVAILABLE errno=%d\n",errno);
            break;
        case BT_OP_HCI_CHANNEL_OPEN: {
            if(!parameter || hci<0 || client>=0){errno=EINVAL;break;}
            int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))break;
            stream=pair[0];client=pair[1];
            if(fcntl(stream,F_SETFL,O_NONBLOCK)){close_transport();break;}
            atomic_store(&stop,0);atomic_store(&alive,1);
            int error=pthread_create(&thread,NULL,transport_thread,NULL);
            if(error){errno=error;close_transport();break;}
            thread_created=1;
            for(unsigned i=0;i<HCI_MAX_CHANNEL;++i)((int *)parameter)[i]=client;
            result=1;break;
        }
        case BT_OP_INIT:
            if(callbacks)notify=callbacks->initCb;
            if(hci>=0 && client>=0 && atomic_load(&alive)){status=BTC_OP_RESULT_SUCCESS;result=0;}
            else errno=ENODEV;
            break;
        case BT_OP_HCI_CHANNEL_CLOSE:
        case BT_OP_POWER_OFF:close_transport();result=0;break;
        case BT_OP_GET_LPM_TIMER:
            // The original HDI also uses this as its watcher timeout. Keep a
            // bounded idle poll even though hardware LPM commands are unsupported.
            if(!parameter){errno=EINVAL;break;}
            *(uint32_t *)parameter=1000;result=0;break;
        default:errno=ENOTSUP;break; // No invented low-power or wake-lock support.
    }
    pthread_mutex_unlock(&lock);
    if(notify)notify(status);
    return result;
}

static void vendor_close(void)
{
    pthread_mutex_lock(&lock);close_transport();callbacks=NULL;pthread_mutex_unlock(&lock);
}

__attribute__((visibility("default")))
const BtVendorInterfaceT BLUETOOTH_VENDOR_LIB_INTERFACE={
    .size=sizeof(BtVendorInterfaceT),.init=vendor_init,.op=vendor_op,.close=vendor_close
};
