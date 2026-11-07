/* Observe real original system abilities; optionally publish a private ready flag. */
#include "iservice_registry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static double now()
{
    timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}

int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);
    unsigned wait=0;const char *publish=nullptr;std::vector<int> ids;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--wait") && i+1<argc){
            char *end;unsigned long n=strtoul(argv[++i],&end,10);
            if(*end || n>120)return 2;wait=(unsigned)n;
        }else if(!strcmp(argv[i],"--publish") && i+1<argc){
            publish=argv[++i];if(strncmp(publish,"aurora.boot.",12))return 2;
        }else{
            char *end;long n=strtol(argv[i],&end,10);
            if(*end || n<=0 || n>2147483647)return 2;ids.push_back((int)n);
        }
    }
    if(ids.empty())return 2;
    alarm(wait+15);double deadline=now()+wait;
    auto manager=OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if(!manager)return 2;
    do{
        bool ready=true;
        for(int id:ids)if(!manager->CheckSystemAbility(id))ready=false;
        if(ready){
            printf("SA_READY_OBSERVED monotonic=%.6f",now());
            for(int id:ids)printf(" id=%d",id);putchar('\n');
            if(publish){
                void *lib=dlopen("/system/lib64/chipset-sdk-sp/libbegetutil.z.so",RTLD_NOW|RTLD_LOCAL);
                auto set=lib?(int(*)(const char*,const char*))dlsym(lib,"SystemSetParameter"):nullptr;
                if(!set || set(publish,"true"))return 2;
            }
            puts("SA_SERVICE_READY_PASS");return 0;
        }
        if(now()>=deadline)break;
        usleep(200000);
    }while(true);
    puts("SA_SERVICE_READY_TIMEOUT");return 2;
}
