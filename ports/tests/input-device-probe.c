/* Original OHOS public API device enumeration; no generated device list. */
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
static atomic_int keys,mouse;
static int (*keycode)(const void *),(*keyaction)(const void *),(*mouseaction)(const void *);
static void key_callback(const void *event)
{ printf("MMI_KEY_EVENT code=%d action=%d\n",keycode(event),keyaction(event));atomic_fetch_add(&keys,1); }
static void mouse_callback(const void *event)
{ printf("MMI_MOUSE_EVENT action=%d\n",mouseaction(event));atomic_fetch_add(&mouse,1); }
int main(void)
{
    setbuf(stdout,NULL); alarm(12);
    void *lib=dlopen("/system/lib64/ndk/libohinput.so",RTLD_NOW|RTLD_LOCAL);
    if (!lib) { puts(dlerror()); return 2; }
    int (*ids)(int32_t *,int32_t,int32_t *)=dlsym(lib,"OH_Input_GetDeviceIds");
    if (!ids) { puts("MMI_DEVICE_MISSING_API"); return 2; }
    int32_t devices[64], count=0;
    int ret=ids(devices,64,&count);
    printf("MMI_DEVICE_ENUM result=%d count=%d\n",ret,count);
    if (ret || count<=0 || count>64) return 2;
    for (int i=0;i<count;++i) printf("MMI_DEVICE_ID=%d\n",devices[i]);
    puts("MMI_DEVICE_ENUM_PASS");
    if (getenv("HMOS_INPUT_EVENTS")) {
        int (*add_key)(void (*)(const void *))=dlsym(lib,"OH_Input_AddKeyEventMonitor");
        int (*add_mouse)(void (*)(const void *))=dlsym(lib,"OH_Input_AddMouseEventMonitor");
        int (*remove_key)(void (*)(const void *))=dlsym(lib,"OH_Input_RemoveKeyEventMonitor");
        int (*remove_mouse)(void (*)(const void *))=dlsym(lib,"OH_Input_RemoveMouseEventMonitor");
        keycode=dlsym(lib,"OH_Input_GetKeyEventKeyCode");keyaction=dlsym(lib,"OH_Input_GetKeyEventAction");
        mouseaction=dlsym(lib,"OH_Input_GetMouseEventAction");
        if (!add_key||!add_mouse||!remove_key||!remove_mouse||!keycode||!keyaction||!mouseaction) return 2;
        int k=add_key(key_callback),m=add_mouse(mouse_callback);
        printf("MMI_MONITOR_RESULT key=%d mouse=%d\n",k,m);
        if (k||m) { if (!k) remove_key(key_callback);if (!m) remove_mouse(mouse_callback);return 2; }
        puts("MMI_INPUT_READY");
        for (int n=0;n<50 && (atomic_load(&keys)<2||atomic_load(&mouse)<1);++n) usleep(100000);
        remove_key(key_callback);remove_mouse(mouse_callback);
        printf("MMI_INPUT_COUNTS keys=%d mouse=%d\n",atomic_load(&keys),atomic_load(&mouse));
        if (atomic_load(&keys)>=2) puts("MMI_KEY_EVENTS_PASS");
        if (atomic_load(&keys)<2||atomic_load(&mouse)<1) return 2;
        puts("MMI_INPUT_EVENTS_PASS");
    }
    return 0;
}
