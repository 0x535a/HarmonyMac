/* Public scalar battery/time APIs, no raw power-supply or clock writes. */
#include <BasicServicesKit/ohbattery_info.h>
#include <BasicServicesKit/time_service.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    unsigned samples=argc>1?(unsigned)strtoul(argv[1],NULL,10):80;
    if(samples<1||samples>600)return 2;
    setbuf(stdout,NULL);alarm(samples/5+30);
    printf("BATTERY_TIME_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    FILE *f=fopen("/proc/self/status","r");if(f){char line[256];while(fgets(line,sizeof(line),f))if(!strncmp(line,"CapEff:",7))fputs(line,stdout);fclose(f);}
    void *battery=dlopen("/system/lib64/ndk/libohbattery_info.so",RTLD_NOW|RTLD_LOCAL);
    void *time=dlopen("/system/lib64/ndk/libtime_service_ndk.so",RTLD_NOW|RTLD_LOCAL);
    void *ffi=dlopen("/system/lib64/platformsdk/libcj_battery_info_ffi.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!battery||!time||!ffi){printf("BATTERY_TIME_DLOPEN_FAILED %s\n",dlerror());return 2;}
    int32_t (*capacity)(void)=dlsym(battery,"OH_BatteryInfo_GetCapacity");
    BatteryInfo_BatteryPluggedType (*plugged)(void)=dlsym(battery,"OH_BatteryInfo_GetPluggedType");
    TimeService_ErrCode (*zone)(char *,uint32_t)=dlsym(time,"OH_TimeService_GetTimeZone");
    // Actual product C-FFI thunks forward scalar values from the unchanged
    // BatterySrvClient; no private C++ object layout crosses this boundary.
    int32_t (*charging)(void)=dlsym(ffi,"FfiBatteryInfoGetChargingState");
    bool (*present)(void)=dlsym(ffi,"FfiBatteryInfoGetBatteryPresent");
    int32_t (*temperature)(void)=dlsym(ffi,"FfiBatteryInfoGetBatteryTemperature");
    int32_t (*voltage)(void)=dlsym(ffi,"FfiBatteryInfoGetVoltage");
    if(!capacity||!plugged||!zone||!charging||!present||!temperature||!voltage){puts("BATTERY_TIME_PUBLIC_API_MISSING");return 2;}
    char timezone[128]={0};int z=zone(timezone,sizeof(timezone));int bad=zone(NULL,0);
    printf("TIME_PUBLIC_ZONE result=%d value=%s invalid_result=%d\n",z,timezone,bad);
    if(z!=TIMESERVICE_ERR_OK||bad!=TIMESERVICE_ERR_INVALID_PARAMETER)return 2;
    puts("BATTERY_TIME_PROBE_READY");
    for(unsigned i=0;i<samples;++i){
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        printf("BATTERY_PUBLIC_SAMPLE i=%u mono=%lld.%09ld capacity=%d plugged=%d\n",i,
            (long long)now.tv_sec,now.tv_nsec,capacity(),plugged());
        printf("BATTERY_FRAMEWORK_SCALARS i=%u present=%d charging=%d temperature=%d voltage=%d\n",
            i,present(),charging(),temperature(),voltage());
        usleep(200000);
    }
    puts("BATTERY_TIME_PROBE_FINISHED");return 0;
}
