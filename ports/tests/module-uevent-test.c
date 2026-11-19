/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../hardware/module-uevent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    const char event[]="add@/devices/pci0000:00/0000:00:01.0\0ACTION=add\0MODALIAS=pci:v00001AF4d00001004\0";
    struct AuroraNetlinkAddress source={.nl_family=AF_NETLINK,.nl_pid=0};
    struct ucred credentials={.pid=0,.uid=0,.gid=0};
    const char *alias=AuroraModuleUevent(event,sizeof(event),&source,&credentials,0);
    assert(alias && !strcmp(alias,"pci:v00001AF4d00001004"));
    source.nl_pid=100;assert(!AuroraModuleUevent(event,sizeof(event),&source,&credentials,0));source.nl_pid=0;
    credentials.pid=100;assert(!AuroraModuleUevent(event,sizeof(event),&source,&credentials,0));credentials.pid=0;
    credentials.uid=1000;assert(!AuroraModuleUevent(event,sizeof(event),&source,&credentials,0));credentials.uid=0;
    assert(!AuroraModuleUevent(event,sizeof(event),&source,NULL,0));
    assert(!AuroraModuleUevent(event,sizeof(event),&source,&credentials,MSG_TRUNC));
    assert(!AuroraModuleUevent(event,sizeof(event),&source,&credentials,MSG_CTRUNC));
    assert(!AuroraModuleUevent(event,sizeof(event)-2,&source,&credentials,0));
    const char duplicate[]="ACTION=add\0ACTION=change\0MODALIAS=x\0";
    assert(!AuroraModuleUevent(duplicate,sizeof(duplicate),&source,&credentials,0));
    const char unsafe[]="ACTION=add\0MODALIAS=../../untrusted\0";
    assert(!AuroraModuleUevent(unsafe,sizeof(unsafe),&source,&credentials,0));
    const char removed[]="ACTION=remove\0MODALIAS=x\0";
    assert(!AuroraModuleUevent(removed,sizeof(removed),&source,&credentials,0));
    puts("MODULE_UEVENT_PARSE_PASS synthetic_inputs=1 userspace_sender_rejected=1");
    return 0;
}
