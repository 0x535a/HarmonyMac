/* Official OHOS C Wi-Fi API over original SA1120/1124; no replacement ability. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include "wifi_device.h"

int main(int argc,char **argv)
{
    int dhcp_test=argc==2 && !strcmp(argv[1],"--dhcp-test");
    int static_ip_test=argc==2 && !strcmp(argv[1],"--static-ip-test");
    int connect_test=dhcp_test || static_ip_test || (argc==2 && !strcmp(argv[1],"--connect-test"));
    int state_only=argc==2 && !strcmp(argv[1],"--state-only");
    int clock_stale=argc==2 && !strcmp(argv[1],"--clock-stale-test");
    if(argc!=1 && !connect_test && !state_only && !clock_stale)return 2;
    if(connect_test || clock_stale) {
        char command[4096]={0};FILE *f=fopen("/proc/cmdline","r");if(!f)return 2;
        size_t n=fread(command,1,sizeof(command)-1,f);fclose(f);
        if(!n || !strstr(command,"aurora.virtual_identity=1"))return 2;
    }
    setbuf(stdout,NULL);alarm(180);printf("WIFI_NATIVE_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    printf("WIFI_NATIVE_MODE dhcp_test=%d connect_test=%d state_only=%d\n",dhcp_test,connect_test,state_only);
    if(static_ip_test)puts("WIFI_NATIVE_STATIC_IP_PAYLOAD_TEST");
    const char *paths[]={"/system/lib64/libwifi_sdk.z.so","/system/lib64/platformsdk/libwifi_sdk.z.so",
        "/system/lib64/chipset-sdk/libwifi_sdk.z.so"};void *lib=NULL;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]) && !lib;++i)lib=dlopen(paths[i],RTLD_NOW|RTLD_LOCAL);
    if(!lib) { printf("WIFI_NATIVE_LIBRARY_FAIL %s\n",dlerror());return 2; }
#define GET(name) __typeof__(&name) name##_fn=(__typeof__(&name))dlsym(lib,#name);if(!name##_fn){printf("WIFI_NATIVE_SYMBOL_FAIL %s\n",#name);return 2;}
    GET(EnableWifi);GET(DisableWifi);GET(IsWifiActive);GET(Scan);GET(GetScanInfoList);
    GET(AddDeviceConfig);GET(GetDeviceConfigs);GET(ConnectTo);GET(GetLinkedInfo);GET(Disconnect);GET(RemoveDevice);
#undef GET
    int initial=IsWifiActive_fn();printf("WIFI_NATIVE_INITIAL active=%d\n",initial);
    if(state_only) {
        WifiDeviceConfig *saved=calloc(WIFI_MAX_CONFIG_SIZE,sizeof(*saved));if(!saved)return 2;
        unsigned count=WIFI_MAX_CONFIG_SIZE;int rc=GetDeviceConfigs_fn(saved,&count),found=0;
        if(!rc && count<=WIFI_MAX_CONFIG_SIZE)for(unsigned i=0;i<count;++i)
            if(!strncmp(saved[i].ssid,"Aurora-Isolated-Test",sizeof(saved[i].ssid)))found=1;
        WifiLinkedInfo info={0};int linked=GetLinkedInfo_fn(&info);
        // A disabled original STA service reports NOT_STARTED for GetLinkedInfo;
        // that documented result is expected here, without enabling the radio.
        int link_clear=(!linked && info.connState==WIFI_DISCONNECTED) ||
            (initial==0 && linked==ERROR_WIFI_NOT_STARTED);
        int result=initial!=0 || rc || count>WIFI_MAX_CONFIG_SIZE || found || !link_clear;
        printf("WIFI_NATIVE_STATE_ONLY configs_result=%d configs_count=%u test_config_present=%d linked_result=%d connected=%d\n",
            rc,count,found,linked,info.connState);
        printf("WIFI_NATIVE_STATE_ONLY_EXIT=%d\n",result?2:0);free(saved);return result?2:0;
    }
    int enabled=EnableWifi_fn();printf("WIFI_NATIVE_ENABLE result=%d\n",enabled);
    if(enabled && enabled!=ERROR_WIFI_BUSY)return 2;
    int active=0;
    for(unsigned i=0;i<30;++i) {
        active=IsWifiActive_fn();if(active==1)break;usleep(500000);
    }
    printf("WIFI_NATIVE_AFTER_ENABLE active=%d\n",active);
    int result=2;
    int64_t first_timestamp=0;
    if(active==1) {
        int scan=Scan_fn();printf("WIFI_NATIVE_SCAN result=%d\n",scan);
        WifiScanInfo *entries=calloc(WIFI_SCAN_HOTSPOT_LIMIT,sizeof(*entries));
        if(entries) {
            for(unsigned attempt=0;attempt<40 && result;++attempt) {
                unsigned count=WIFI_SCAN_HOTSPOT_LIMIT;int listed=GetScanInfoList_fn(entries,&count);
                if(attempt%4==0 || count)printf("WIFI_NATIVE_SCAN_LIST attempt=%u result=%d count=%u\n",attempt,listed,count);
                if(!listed && count<=WIFI_SCAN_HOTSPOT_LIMIT)for(unsigned i=0;i<count;++i) {
                    if(!strncmp(entries[i].ssid,"Aurora-Isolated-Test",sizeof(entries[i].ssid))) {
                        printf("WIFI_NATIVE_EXPECTED_AP ssid=%.*s frequency=%d rssi=%d security=%d\n",
                            (int)sizeof(entries[i].ssid),entries[i].ssid,entries[i].frequency,entries[i].rssi,entries[i].securityType);
                        if(!scan && entries[i].frequency==2412){result=0;first_timestamp=entries[i].timestamp;}
                    }
                }
                if(result)usleep(500000);
            }
            free(entries);
        }
    }
    int scan_match=result==0;
    if(scan_match)puts("WIFI_NATIVE_SCAN_MATCH_PASS");
    if(clock_stale && scan_match) {
        struct timespec now={0};clock_gettime(CLOCK_BOOTTIME,&now);
        int64_t now_us=(int64_t)now.tv_sec*1000000+now.tv_nsec/1000;
        printf("WIFI_NATIVE_CLOCK_FRESH timestamp_us=%lld boot_us=%lld\n",(long long)first_timestamp,(long long)now_us);
        if(first_timestamp<=0 || first_timestamp>now_us+5000)result=2;
        puts("WIFI_NATIVE_CLOCK_STALE_READY");
        unsigned waited=0;
        while(access("/data/boot/aurora/wifi-native-probe/clock-supervisor-done",F_OK) && waited++<100)usleep(250000);
        if(access("/data/boot/aurora/wifi-native-probe/clock-supervisor-done",F_OK))result=2;
        clock_gettime(CLOCK_BOOTTIME,&now);int64_t request_us=(int64_t)now.tv_sec*1000000+now.tv_nsec/1000;
        int rc=Scan_fn();printf("WIFI_NATIVE_CLOCK_STALE_SCAN result=%d request_boot_us=%lld\n",rc,(long long)request_us);
        if(rc)result=2;
        sleep(6);
        WifiScanInfo *entries=calloc(WIFI_SCAN_HOTSPOT_LIMIT,sizeof(*entries));
        unsigned count=WIFI_SCAN_HOTSPOT_LIMIT;int64_t newest=0;
        if(!entries)result=2;
        else {
            rc=GetScanInfoList_fn(entries,&count);
            if(rc || count>WIFI_SCAN_HOTSPOT_LIMIT)result=2;
            else for(unsigned i=0;i<count;++i)if(!strncmp(entries[i].ssid,"Aurora-Isolated-Test",sizeof(entries[i].ssid))) {
                if(entries[i].timestamp>newest)newest=entries[i].timestamp;
                if(entries[i].timestamp>=request_us-1000000)result=2;
            }
            free(entries);
        }
        printf("WIFI_NATIVE_CLOCK_STALE_CHECK result=%d cached_count=%u newest_us=%lld cutoff_us=%lld\n",result,count,(long long)newest,(long long)(request_us-1000000));
        if(!result)puts("WIFI_NATIVE_CLOCK_STALE_PASS");
    }
    if(connect_test && scan_match) {
        result=2;int existing=0;
        WifiDeviceConfig *saved=calloc(WIFI_MAX_CONFIG_SIZE,sizeof(*saved));
        if(saved) {
            unsigned count=WIFI_MAX_CONFIG_SIZE;
            int rc=GetDeviceConfigs_fn(saved,&count);
            printf("WIFI_NATIVE_CONFIG_INVENTORY result=%d count=%u\n",rc,count);
            if(rc || count>WIFI_MAX_CONFIG_SIZE)existing=1;
            else for(unsigned i=0;i<count;++i)if(!strncmp(saved[i].ssid,"Aurora-Isolated-Test",sizeof(saved[i].ssid)))existing=1;
            free(saved);
        } else existing=1;
        int id=-1;
        if(!existing) {
            WifiDeviceConfig config={0};
            snprintf(config.ssid,sizeof(config.ssid),"Aurora-Isolated-Test");
            snprintf(config.preSharedKey,sizeof(config.preSharedKey),"aurora-virtual-test-only");
            config.securityType=WIFI_SEC_TYPE_PSK;config.netId=-1;config.freq=2412;
            config.ipType=dhcp_test?DHCP:STATIC_IP;config.randomMacType=0;
            // The public C API stores IPv4 as a numeric address (0xc0000202),
            // unlike the network-order integer returned by inet_addr on ARM64.
            config.staticIp.ipAddress=ntohl(inet_addr("192.0.2.2"));config.staticIp.gateway=ntohl(inet_addr("192.0.2.1"));
            config.staticIp.netmask=ntohl(inet_addr("255.255.255.0"));config.staticIp.dnsServers[0]=ntohl(inet_addr("192.0.2.1"));
            if(dhcp_test)memset(&config.staticIp,0,sizeof(config.staticIp));
            int rc=AddDeviceConfig_fn(&config,&id);printf("WIFI_NATIVE_ADD result=%d id=%d static_ip_test=%d dhcp_test=%d\n",rc,id,!dhcp_test,dhcp_test);
            if(!rc && id>=0) {
                rc=ConnectTo_fn(id);printf("WIFI_NATIVE_CONNECT_REQUEST result=%d\n",rc);
                if(!rc)for(unsigned i=0;i<40;++i) {
                    WifiLinkedInfo info={0};int got=GetLinkedInfo_fn(&info);
                    if(i%4==0 || info.connState==WIFI_CONNECTED)
                        printf("WIFI_NATIVE_LINK attempt=%u result=%d state=%d frequency=%d ip=%08x ssid=%.*s\n",
                            i,got,info.connState,info.frequency,info.ipAddress,(int)sizeof(info.ssid),info.ssid);
                    bool ip_matches=true;
                    if(dhcp_test)ip_matches=(info.ipAddress&0xffffff00u)==0xc0000200u &&
                        (info.ipAddress&255)>=20 && (info.ipAddress&255)<=30;
                    if(static_ip_test)ip_matches=info.ipAddress==config.staticIp.ipAddress;
                    if(!got && info.connState==WIFI_CONNECTED && info.frequency==2412 &&
                        !strncmp(info.ssid,config.ssid,sizeof(info.ssid)) && ip_matches) {result=0;break;}
                    usleep(500000);
                }
                if(!result && (dhcp_test || static_ip_test)) {
                    char nonce[64]={0};FILE *request=fopen("/data/boot/aurora/wifi-native-probe/ip-test-nonce","r");
                    if(!request || !fgets(nonce,sizeof(nonce),request)){if(request)fclose(request);result=2;}
                    else {
                        fclose(request);nonce[strcspn(nonce,"\r\n")]=0;
                        pid_t child=fork();
                        if(!child) {
                            execl("/system/bin/aurora/wifi-ip/network-traffic-probe","network-traffic-probe", "49173",nonce,"wlan0","192.0.2.1","dns",(char *)NULL);_exit(2);
                        }
                        int status=0;pid_t waited;
                        do {waited=child>0?waitpid(child,&status,0):-1;}while(child>0 && waited<0 && errno==EINTR);
                        int code=child>0 && waited==child && WIFEXITED(status)?WEXITSTATUS(status):2;
                        printf("WIFI_NATIVE_NETWORK_TRAFFIC_EXIT=%d\n",code);if(code)result=2;
                    }
                }
                printf("WIFI_NATIVE_CONNECTED_HOLD result=%d\n",result);
                for(unsigned i=0;i<35 && access("/data/boot/aurora/wifi-native-probe/supervisor-done",F_OK);++i)sleep(1);
                int disconnected=Disconnect_fn();int removed=RemoveDevice_fn(id);
                printf("WIFI_NATIVE_CONFIG_CLEANUP disconnect=%d remove=%d id=%d\n",disconnected,removed,id);
                if(disconnected || removed)result=2;
                WifiLinkedInfo info={0};int got=-1;
                for(unsigned i=0;i<20;++i) {
                    got=GetLinkedInfo_fn(&info);if(!got && info.connState==WIFI_DISCONNECTED)break;
                    usleep(250000);
                }
                printf("WIFI_NATIVE_AFTER_DISCONNECT result=%d state=%d\n",got,info.connState);
                if(got || info.connState!=WIFI_DISCONNECTED)result=2;
            }
        } else puts("WIFI_NATIVE_CONFIG_REFUSE_EXISTING_OR_UNREADABLE");
        printf("WIFI_NATIVE_CONNECT_TEST_EXIT=%d\n",result);
    }
    printf("WIFI_NATIVE_DIAGNOSTIC_HOLD result=%d\n",result);sleep(5);
    if(initial!=1) {
        int disabled=DisableWifi_fn();printf("WIFI_NATIVE_RESTORE_DISABLE result=%d\n",disabled);
        int final=-1;for(unsigned i=0;i<20;++i){final=IsWifiActive_fn();if(final==0)break;usleep(250000);}
        printf("WIFI_NATIVE_FINAL_ACTIVE=%d\n",final);if(disabled || final!=0)result=2;
    }
    printf("WIFI_NATIVE_PROBE_EXIT=%d original_api_scan_match=%d physical_rf_verified=0\n",result,scan_match);
    return result;
}
