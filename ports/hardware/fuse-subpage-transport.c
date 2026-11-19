/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Service-local exFAT transport for Aurora's 4K userspace on a 16K kernel.
 * FUSE's read/write iterator uses GUP, which deliberately rejects subpage mm.
 * Copy through ordinary pipes; splice operates on native kernel pages. Never
 * use vmsplice or relax the kernel's page ownership checks. Other fds go
 * straight to the real syscalls. Load only into the preserved exFAT helper.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

static int is_fuse(int fd)
{
    struct stat st;
    int saved=errno;
    int result=!fstat(fd,&st) && S_ISCHR(st.st_mode) &&
        major(st.st_rdev)==10 && minor(st.st_rdev)==229;
    errno=saved;
    return result;
}

static int staging_pipe(int p[2],size_t bytes)
{
    if (bytes>INT_MAX) { errno=EMSGSIZE;return -1; }
    if (pipe2(p,O_CLOEXEC)) return -1;
    int size=fcntl(p[0],F_GETPIPE_SZ);
    if (size<0 || ((size_t)size<bytes && fcntl(p[0],F_SETPIPE_SZ,(int)bytes)<0)) {
        int saved=errno;close(p[0]);close(p[1]);errno=saved;return -1;
    }
    return 0;
}

static ssize_t finish(int p[2],ssize_t result)
{
    int saved=errno;close(p[0]);close(p[1]);errno=saved;return result;
}

ssize_t read(int fd,void *buffer,size_t count)
{
    if (!count || !is_fuse(fd)) return syscall(SYS_read,fd,buffer,count);
    int p[2];
    if (staging_pipe(p,count)) return -1;
    ssize_t result=syscall(SYS_splice,fd,NULL,p[1],NULL,count,0);
    if (result<=0) return finish(p,result);
    /* No writer remains, so an interrupted/short copy cannot block waiting
     * for additional bytes. Do not splice another FUSE request into this one. */
    close(p[1]);p[1]=-1;
    size_t done=0;
    while (done<(size_t)result) {
        ssize_t n=syscall(SYS_read,p[0],(char *)buffer+done,(size_t)result-done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { if (!n) errno=EIO;return finish(p,-1); }
        done+=(size_t)n;
    }
    return finish(p,result);
}

ssize_t writev(int fd,const struct iovec *iov,int count)
{
    if (!is_fuse(fd)) return syscall(SYS_writev,fd,iov,count);
    if (count<0 || count>IOV_MAX) { errno=EINVAL;return -1; }
    size_t size=0;
    for (int i=0;i<count;i++) {
        if (iov[i].iov_len>(size_t)SSIZE_MAX-size) { errno=EINVAL;return -1; }
        size+=iov[i].iov_len;
    }
    if (!size) return syscall(SYS_writev,fd,iov,count);
    int p[2];
    if (staging_pipe(p,size)) return -1;
    ssize_t n=syscall(SYS_writev,p[1],iov,count);
    if (n<0) return finish(p,n);
    if ((size_t)n!=size) { errno=EIO;return finish(p,-1); }
    n=syscall(SYS_splice,p[0],NULL,fd,NULL,size,0);
    return finish(p,n);
}

ssize_t write(int fd,const void *buffer,size_t count)
{
    if (!count || !is_fuse(fd)) return syscall(SYS_write,fd,buffer,count);
    const struct iovec iov={.iov_base=(void *)buffer,.iov_len=count};
    return writev(fd,&iov,1);
}
