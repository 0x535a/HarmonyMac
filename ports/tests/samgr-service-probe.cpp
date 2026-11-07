/* Read-only SA existence checks through the original OHOS client. */
#include "iservice_registry.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);alarm(15);
    auto manager=OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (!manager) {puts("SAMGR_PROBE_NO_MANAGER");return 2;}
    bool passed=true;
    for (int i=1;i<argc;++i) {
        int id=std::atoi(argv[i]);
        auto object=manager->CheckSystemAbility(id);
        printf("SAMGR_SERVICE id=%d present=%d\n",id,object!=nullptr);
        passed &= object!=nullptr;
    }
    if (passed) puts("SAMGR_SERVICE_PROBE_PASS");
    return passed ? 0 : 2;
}
