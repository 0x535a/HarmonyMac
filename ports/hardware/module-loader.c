/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Linux modalias -> libkmod, built for OHOS. Kernel credentials are checked on
 * every hotplug event. No driver unload, forced vermagic or permission bypass.
 */
#define _GNU_SOURCE
#include "module-uevent.h"
#include <libkmod/libkmod.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct kmod_ctx *context;
static bool dry_run;
static volatile sig_atomic_t stopping;
static unsigned examined,matched,inserted,existing,failed,blocked,unmatched;
static FILE *audit;

static void message(const char *alias,const char *module,int result)
{
    if (!audit) return;
    struct stat s;
    if (!fstat(fileno(audit),&s) && s.st_size>2*1024*1024) {
        fflush(audit);ftruncate(fileno(audit),0);fseek(audit,0,SEEK_SET);
    }
    fprintf(audit,"MODULE_REQUEST alias=%s module=%s result=%d dry_run=%d\n",alias,module,result,dry_run);
    fflush(audit);
}

static int request(const char *alias)
{
    struct kmod_list *list=NULL,*entry;
    int result=kmod_module_new_from_lookup(context,alias,&list);
    ++examined;
    if (result<0) { ++failed;message(alias,"lookup",result);return result; }
    if (!list) ++unmatched;
    kmod_list_foreach(entry,list) {
        struct kmod_module *module=kmod_module_get_module(entry);
        int before=kmod_module_get_initstate(module);
        ++matched;
        if (dry_run) {
            message(alias,kmod_module_get_name(module),0);
        } else if (before==KMOD_MODULE_BUILTIN || before==KMOD_MODULE_LIVE || before==KMOD_MODULE_COMING) {
            ++existing;
        } else {
            result=kmod_module_probe_insert_module(module,KMOD_PROBE_APPLY_BLACKLIST,NULL,NULL,NULL,NULL);
            if (result<0) ++failed;
            else if (result>0) ++blocked;
            else if (kmod_module_get_initstate(module)==KMOD_MODULE_LIVE) ++inserted;
            else ++existing;
            message(alias,kmod_module_get_name(module),result);
        }
        kmod_module_unref(module);
    }
    kmod_module_unref_list(list);
    return 0;
}

static int visit(const char *path,const struct stat *info,int type,struct FTW *position)
{
    (void)info;
    if (stopping) return 1;
    if (type!=FTW_F || strcmp(path+position->base,"modalias")) return 0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return 0; /* Device may have disappeared during enumeration. */
    char alias[4096];ssize_t n=read(fd,alias,sizeof(alias)-1);close(fd);
    if (n<=0 || n==(ssize_t)sizeof(alias)-1) return 0;
    while (n && (alias[n-1]=='\n' || alias[n-1]=='\r')) --n;
    alias[n]=0;
    if (!n) return 0;
    for (ssize_t i=0;i<n;++i) if ((unsigned char)alias[i]<=32 || (unsigned char)alias[i]>=127 || alias[i]=='/' || alias[i]=='\\') return 0;
    request(alias);
    return 0;
}

static void scan(void)
{
    int result=nftw("/sys/devices",visit,20,FTW_PHYS|FTW_MOUNT);
    fprintf(stdout,"MODULE_SCAN result=%d examined=%u matched=%u inserted=%u existing=%u unmatched=%u blocked=%u failed=%u dry_run=%d\n",
            result,examined,matched,inserted,existing,unmatched,blocked,failed,dry_run);
    if (audit) {
        fprintf(audit,"MODULE_SCAN result=%d examined=%u matched=%u inserted=%u existing=%u unmatched=%u blocked=%u failed=%u\n",
                result,examined,matched,inserted,existing,unmatched,blocked,failed);fflush(audit);
    }
}
static void stop(int signal_number) { (void)signal_number;stopping=1; }

int main(int argc,char **argv)
{
    bool once=false;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--once")) once=true;
        else if (!strcmp(argv[i],"--dry-run")) dry_run=true;
        else { fprintf(stderr,"usage: module-loader [--once] [--dry-run]\n");return 2; }
    }
    setbuf(stdout,NULL);
    context=kmod_new(NULL,NULL);
    if (!context) return 2;
    int logfd=open("/data/boot/aurora/module-loader.log",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if (logfd>=0) { audit=fdopen(logfd,"a");if(!audit)close(logfd); }
    if (!once) {
        signal(SIGTERM,stop);signal(SIGINT,stop);
    }
    int fd=-1;
    if (!once) {
        fd=socket(AF_NETLINK,SOCK_DGRAM|SOCK_CLOEXEC,AURORA_NETLINK_KOBJECT_UEVENT);
        int yes=1,buffer=1024*1024;
        struct AuroraNetlinkAddress address={.nl_family=AF_NETLINK,.nl_groups=1};
        if (fd<0 || setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes)) ||
            bind(fd,(struct sockaddr *)&address,sizeof(address))) { perror("module netlink");return 2; }
        (void)setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer));
    }
    scan();
    if (once) { if(audit)fclose(audit);kmod_unref(context);return failed?1:0; }
    puts("MODULE_LOADER_LISTENING credentials=kernel_only");
    while (!stopping) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        int ready=poll(&p,1,1000);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0) { perror("module poll");break; }
        if (!ready) continue;
        char data[8192];
        union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(struct ucred))]; } control;
        struct AuroraNetlinkAddress sender={0};
        struct iovec iov={.iov_base=data,.iov_len=sizeof(data)};
        struct msghdr msg={.msg_name=&sender,.msg_namelen=sizeof(sender),.msg_iov=&iov,
            .msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
        ssize_t size=recvmsg(fd,&msg,0);
        if (size<0) { if (errno==ENOBUFS) scan();continue; }
        struct ucred *cred=NULL;
        /* SO_PASSCRED is the sole ancillary option, and the buffer holds
         * exactly one credentials record. Reject truncation/unknown records.
         * Avoid the old SDK's signed-arithmetic CMSG_NXTHDR macro. */
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
        if(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
           c->cmsg_len>=CMSG_LEN(sizeof(struct ucred)) && c->cmsg_len<=msg.msg_controllen)
            cred=(struct ucred *)CMSG_DATA(c);
        const char *alias=AuroraModuleUevent(data,(size_t)size,&sender,cred,msg.msg_flags);
        if (alias) request(alias);
        else if (msg.msg_flags & MSG_TRUNC) scan();
    }
    close(fd);if(audit)fclose(audit);kmod_unref(context);
    return 0;
}
