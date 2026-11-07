/* Read real per-user language through the original OHOS i18n client. */
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);alarm(30);
    int user=argc==2?std::atoi(argv[1]):100;
    if(user<0)return 2;
    void *lib=dlopen("/system/lib64/platformsdk/libi18n_sa_client.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){puts(dlerror());return 2;}
    auto get=(std::string(*)(int))dlsym(lib,
        "_ZN4OHOS6Global4I18n24I18nServiceAbilityClient21GetLanguageFromUserIdEi");
    if(!get)return 2;
    timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    std::string language=get(user);
    clock_gettime(CLOCK_MONOTONIC,&b);
    printf("I18N_LANGUAGE user=%d value=%s elapsed_ms=%.3f\n",user,language.c_str(),
           (b.tv_sec-a.tv_sec)*1000.+(b.tv_nsec-a.tv_nsec)/1e6);
    if(language.empty())return 2;
    puts("I18N_LANGUAGE_PASS");return 0;
}
