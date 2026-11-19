/* SPDX-License-Identifier: Apache-2.0
 * Typed public HDI query through the installed stock proxy/wrapper.
 */
#include "v3_0/isensor_interface.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
using namespace OHOS::HDI::Sensor::V3_0;
int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);
    bool absent=argc==2 && !strcmp(argv[1],"--absent");
    if(argc!=2 || (!absent && strcmp(argv[1],"--watch")) || getuid()!=2000)return 2;
    auto api=ISensorInterface::Get();if(!api){puts("SENSOR_HDI_CONNECTION_FAILED");return 2;}
    uint32_t major=0,minor=0;int result=api->GetVersion(major,minor);
    printf("SENSOR_HDI_VERSION result=%d version=%u.%u uid=%u\n",result,major,minor,getuid());
    if(result || major!=3 || minor!=0)return 2;
    std::vector<HdfSensorInformation> info;result=api->GetAllSensorInfo(info);
    printf("SENSOR_HDI_INFO result=%d count=%zu\n",result,info.size());
    for(const auto &s:info)printf("SENSOR_HDI_ITEM name=%s type=%d id=%d device=%d minDelay=%lld maxDelay=%lld fifo=%u\n",s.sensorName.c_str(),s.deviceSensorInfo.sensorType,s.deviceSensorInfo.sensorId,s.deviceSensorInfo.deviceId,(long long)s.minDelay,(long long)s.maxDelay,s.fifoMaxEventCount);
    if(!absent && info.size()==1 && (info[0].minDelay!=200000000 || info[0].maxDelay!=10000000000LL))return 2;
    return result || (absent ? !info.empty() : info.size()!=1) ? 2 : 0;
}
