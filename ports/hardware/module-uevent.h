/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AURORA_MODULE_UEVENT_H
#define AURORA_MODULE_UEVENT_H
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
/* Linux sockaddr_nl UAPI. The public OHOS SDK's linux/socket.h conflicts with
 * its libc socket header, so keep this stable transport layout explicit. */
struct AuroraNetlinkAddress {
    sa_family_t nl_family;
    unsigned short nl_pad;
    uint32_t nl_pid,nl_groups;
};
_Static_assert(sizeof(struct AuroraNetlinkAddress)==12,"Linux netlink address ABI");
#define AURORA_NETLINK_KOBJECT_UEVENT 15
/* Returns an alias inside the bounded message only for authenticated kernel
 * add/change events. No filesystem paths or commands are taken from an event. */
const char *AuroraModuleUevent(const char *data, size_t len,
    const struct AuroraNetlinkAddress *sender, const struct ucred *credentials, int flags);
#endif
