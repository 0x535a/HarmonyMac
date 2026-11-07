/* Early-boot evdev DAC setup; no dependency on optional BusyBox applets. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
/* Linux evdev UAPI: EVIOCGNAME(len) = _IOC(_IOC_READ, 'E', 0x06, len). */
#define EVIOCGNAME(length) _IOC(_IOC_READ, 'E', 0x06, length)
int main(int argc,char **argv)
{
    if (argc!=3) return 2;
    char *end; unsigned long uid=strtoul(argv[1],&end,10); if (*end) return 2;
    unsigned long gid=strtoul(argv[2],&end,10); if (*end) return 2;
    DIR *dir=opendir("/dev/input");
    if (!dir) { perror("INPUT_DAC opendir"); return 1; }
    struct dirent *e; int count=0,errors=0;
    while ((e=readdir(dir))) {
        if (strncmp(e->d_name,"event",5)) continue;
        char path[300],name[256]={0};snprintf(path,sizeof(path),"/dev/input/%s",e->d_name);
        struct stat st;if (lstat(path,&st)||!S_ISCHR(st.st_mode)) { ++errors;continue; }
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd>=0) { ioctl(fd,EVIOCGNAME(sizeof(name)),name);close(fd); }
        if (chown(path,(uid_t)uid,(gid_t)gid)||chmod(path,0660)) { perror(path);++errors;continue; }
        printf("INPUT_DAC_READY path=%s name=%s uid=%lu gid=%lu\n",path,name,uid,gid);++count;
    }
    closedir(dir);printf("INPUT_DAC_COUNT=%d errors=%d\n",count,errors);
    return errors || !count;
}
