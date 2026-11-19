/* Original platform C ABI, checked against communication_bluetooth OH6 commit
 * fe1240cf65788e86e7e3c6850a39677d3545d6ce, bluetooth_access_ffi.h. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef void (*Switch)(int32_t *);
typedef int32_t (*State)(int32_t *);
static int get_state(State state)
{
    int32_t error=-999;int32_t value=state(&error);
    printf("BLUETOOTH_API_STATE value=%d error=%d\n",value,error);
    return error?-1:value;
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);alarm(85);
    int classic=argc==2 && !strcmp(argv[1],"--classic");
    int missing=argc==2 && !strcmp(argv[1],"--expect-missing");
    if(argc!=1 && !classic && !missing)return 2;
    char cmdline[4096]={0};FILE *f=fopen("/proc/cmdline","r");if(!f)return 2;
    size_t n=fread(cmdline,1,sizeof(cmdline)-1,f);fclose(f);
    if(!n || !strstr(cmdline,"aurora.virtual_identity=1"))return 2;
    printf("BLUETOOTH_API_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    void *lib=dlopen("/system/lib64/platformsdk/libcj_bluetooth_access_ffi.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("BLUETOOTH_API_DLOPEN_FAIL %s\n",dlerror());return 2;}
    Switch enable=(Switch)dlsym(lib,"FfiBluetoothAccEnableBluetooth");
    Switch disable=(Switch)dlsym(lib,"FfiBluetoothAccDisableBluetooth");
    State state=(State)dlsym(lib,"FfiBluetoothAccGetState");
    if(!enable || !disable || !state){puts("BLUETOOTH_API_SYMBOL_FAIL");return 2;}
    printf("BLUETOOTH_API_MODE classic=%d\n",classic);
    int initial=get_state(state),result=2;
    if(initial!=0){puts("BLUETOOTH_API_REFUSE_INITIAL_ACTIVE");return 2;}
    if(missing) {
        DIR *dir=opendir("/sys/class/bluetooth");
        if(dir) {
            struct dirent *entry;int present=0;
            while((entry=readdir(dir)))if(!strncmp(entry->d_name,"hci",3))present=1;
            closedir(dir);if(present)return 2;
        }
        for(unsigned cycle=0;cycle<2;++cycle) {
            int32_t error=-999;enable(&error);
            printf("BLUETOOTH_API_MISSING_ENABLE cycle=%u error=%d\n",cycle,error);
            if(error && (error<2900000 || error>2909999))return 2;
            sleep(2);int observed=-1;
            for(unsigned i=0;i<40;++i) {
                observed=get_state(state);if(observed==0)break;
                if(observed==2 || observed==5)return 2;
                usleep(250000);
            }
            if(observed!=0)return 2;
            sleep(2);if(get_state(state)!=0)return 2;
        }
        puts("BLUETOOTH_API_MISSING_CONTROLLER_PASS cycles=2 actual_state=OFF");return 0;
    }
    for(unsigned cycle=0;cycle<2;++cycle) {
        // Use the public master switch. The retained server explicitly rejects
        // deprecated native EnableBt/DisableBle; do not use them as substitutes.
        int32_t error=-999;enable(&error);
        printf("BLUETOOTH_API_ENABLE cycle=%u error=%d\n",cycle,error);
        if(error)goto cleanup;
        int observed=-1;
        for(unsigned i=0;i<40;++i) {
            observed=get_state(state);
            if(observed==2 || (!classic && observed==5))break;
            usleep(500000);
        }
        if(observed!=2 && (classic || observed!=5))goto cleanup;
        printf("BLUETOOTH_API_ACTIVE cycle=%u state=%d classic_enabled=%d\n",cycle,observed,observed==2);
        sleep(2);
        error=-999;disable(&error);printf("BLUETOOTH_API_DISABLE cycle=%u error=%d\n",cycle,error);
        if(error)goto cleanup;
        for(unsigned i=0;i<40;++i) {
            observed=get_state(state);if(observed==0)break;usleep(500000);
        }
        if(observed!=0)goto cleanup;
        printf("BLUETOOTH_API_OFF cycle=%u\n",cycle);
    }
    result=0;
cleanup:
    if(get_state(state)!=0) {
        int32_t error=-999;disable(&error);printf("BLUETOOTH_API_RESTORE_DISABLE error=%d\n",error);
        if(error)result=2;
        if(!error) {
            int observed=-1;
            for(unsigned i=0;i<40;++i) {
                observed=get_state(state);if(observed==0)break;usleep(250000);
            }
            printf("BLUETOOTH_API_RESTORE_STATE=%d\n",observed);
            if(observed!=0)result=2;
        }
    }
    printf("BLUETOOTH_API_PROBE_EXIT=%d physical_rf_verified=0\n",result);
    return result;
}
