/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "module-uevent.h"
#include <string.h>

const char *AuroraModuleUevent(const char *data,size_t len,
    const struct AuroraNetlinkAddress *sender,const struct ucred *cred,int flags)
{
    if (!data || !len || data[len-1] || !sender || !cred ||
        sender->nl_family!=AF_NETLINK || sender->nl_pid!=0 ||
        cred->pid!=0 || cred->uid!=0 || cred->gid!=0 ||
        (flags & (MSG_TRUNC|MSG_CTRUNC))) return NULL;
    const char *action=NULL,*alias=NULL;
    size_t pos=0;
    while (pos<len) {
        const char *end=memchr(data+pos,0,len-pos);
        if (!end) return NULL;
        size_t size=(size_t)(end-(data+pos));
        if (size>=7 && !memcmp(data+pos,"ACTION=",7)) {
            if (action) return NULL;
            action=data+pos+7;
        }
        if (size>=9 && !memcmp(data+pos,"MODALIAS=",9)) {
            if (alias) return NULL;
            alias=data+pos+9;
        }
        pos+=(size+1);
    }
    if (!action || (strcmp(action,"add") && strcmp(action,"change")) || !alias || !*alias) return NULL;
    for (const unsigned char *p=(const unsigned char *)alias;*p;++p)
        if (*p<=32 || *p>=127 || *p=='/' || *p=='\\') return NULL;
    return alias;
}
