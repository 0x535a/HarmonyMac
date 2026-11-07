/* Diagnostic only: collect original DFX stacks when real HiLog reports a RenderService block. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec+t.tv_nsec/1e9;
}

static void snapshot(long pid, const char *name)
{
    char from[160],to[200],chunk[8192];
    snprintf(from,sizeof(from),"/proc/%ld/%s",pid,name);
    snprintf(to,sizeof(to),"/data/boot/aurora/rs-block-proc-%ld-%s",pid,name);
    int input=open(from,O_RDONLY),output=open(to,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(input>=0 && output>=0){
        ssize_t count;
        while((count=read(input,chunk,sizeof(chunk)))>0){
            ssize_t offset=0;
            while(offset<count){
                ssize_t n=write(output,chunk+offset,(size_t)(count-offset));
                if(n<=0)break;
                offset+=n;
            }
            if(offset<count)break;
        }
    }
    if(input>=0)close(input);
    if(output>=0)close(output);
}

int main(void)
{
    const char *log="/data/boot/aurora/hilograw.log";
    double deadline=now()+420;
    char buffer[32768];
    size_t retained=0;
    int fd=-1;
    setbuf(stdout,NULL);
    while (fd<0 && now()<deadline) {
        fd=open(log,O_RDONLY);
        if(fd<0)usleep(50000);
    }
    if(fd<0)return 2;
    while(now()<deadline) {
        ssize_t n=read(fd,buffer+retained,sizeof(buffer)-retained-1);
        if(n<0 && errno!=EINTR)return 2;
        if(n<=0){usleep(50000);continue;}
        size_t size=retained+(size_t)n;
        buffer[size]=0;
        int matched=0;
        /* HiLog records are binary; scan every byte rather than strstr. */
        for(size_t i=0;i+14<=size && !matched;i++) {
            if(memcmp(buffer+i,"block happened",14))continue;
            size_t end=i+192<size?i+192:size;
            for(size_t j=i+14;j+13<=end;j++)
                if(!memcmp(buffer+j,"RenderService",13)){matched=1;break;}
        }
        if(matched){
            printf("RS_BLOCK_WATCH_TRIGGER monotonic=%.6f\n",now());
            DIR *dir=opendir("/proc");
            struct dirent *entry;
            if(!dir)return 2;
            while((entry=readdir(dir))){
                char path[128],cmd[256]={0},pid[24],out[160];
                long value=strtol(entry->d_name,NULL,10);
                if(value<=1)continue;
                snprintf(path,sizeof(path),"/proc/%ld/comm",value);
                int proc=open(path,O_RDONLY);
                if(proc<0)continue;
                ssize_t count=read(proc,cmd,sizeof(cmd)-1);close(proc);
                if(count<=0)continue;
                cmd[strcspn(cmd,"\r\n")]=0;
                if(strcmp(cmd,"render_service"))continue;
                snprintf(pid,sizeof(pid),"%ld",value);
                snapshot(value,"maps");snapshot(value,"status");
                snapshot(value,"wchan");snapshot(value,"syscall");
                snapshot(value,"stack");snapshot(value,"stat");
                snprintf(out,sizeof(out),"/data/boot/aurora/rs-block-trigger-%ld.txt",value);
                printf("RS_BLOCK_WATCH_CAPTURE pid=%s monotonic=%.6f\n",pid,now());
                pid_t child=fork();
                if(child==0){
                    int output=open(out,O_WRONLY|O_CREAT|O_TRUNC,0600);
                    if(output<0)_exit(2);
                    dup2(output,STDOUT_FILENO);dup2(output,STDERR_FILENO);close(output);
                    execl("/system/bin/aurora/app-stack-probe","app-stack-probe",pid,"all",(char*)NULL);
                    _exit(127);
                }
                int status=0;
                if(child<0 || waitpid(child,&status,0)<0){closedir(dir);return 2;}
                printf("RS_BLOCK_WATCH_CAPTURE_END status=%d monotonic=%.6f\n",status,now());
            }
            closedir(dir);close(fd);return 0;
        }
        retained=size<256?size:256;
        memmove(buffer,buffer+size-retained,retained);
    }
    puts("RS_BLOCK_WATCH_NO_REPORT");close(fd);return 0;
}
