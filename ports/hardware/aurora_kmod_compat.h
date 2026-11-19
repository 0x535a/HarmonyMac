/* SPDX-License-Identifier: LGPL-2.1-or-later
 * OHOS libc lacks GNU secure_getenv. Never fall back to unguarded getenv in
 * a kernel module tool: preserve privilege-sensitive environment handling.
 */
#ifndef AURORA_KMOD_COMPAT_H
#define AURORA_KMOD_COMPAT_H
#include <errno.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <unistd.h>

static inline char *AuroraSecureGetenv(const char *name)
{
    int saved, error;
    unsigned long secure;
    if (getuid() != geteuid() || getgid() != getegid()) return NULL;
    saved = errno;
    errno = 0;
    secure = getauxval(AT_SECURE);
    error = errno;
    errno = saved;
    if (secure || error) return NULL;
    return getenv(name);
}

/* Exported by the pinned original OHOS musl loader, as on upstream musl. */
extern char *__progname;
#define program_invocation_short_name __progname
#endif
