/* Use the original OHOS DFX client to inspect a real application main thread. */
#include "dfx_dump_catcher.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);alarm(20);if(argc!=2 && argc!=3)return 2;
    int pid=std::atoi(argv[1]);if(pid<=1)return 2;
    OHOS::HiviewDFX::DfxDumpCatcher catcher;
    std::string stack;
    int tid=argc==3 && std::strcmp(argv[2],"all")==0 ? 0 : pid;
    auto result=catcher.DumpCatchWithTimeout(pid,stack,8000,tid,false);
    printf("APP_STACK_RESULT pid=%d result=%d reason=%s\n",pid,result.first,result.second.c_str());
    fwrite(stack.data(),1,stack.size(),stdout);putchar('\n');
    return result.first==0 ? 0 : 2;
}
