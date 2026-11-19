/* Observe genuine QMP/evdev/MMI input using the original public C API. */
#include <stdbool.h>
#include <multimodalinput/oh_input_manager.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int32_t (*get_code)(const struct Input_KeyEvent *);
static int32_t (*get_action)(const struct Input_KeyEvent *);
static _Atomic unsigned down[16],up[16];
static const int codes[16]={KEYCODE_F1,KEYCODE_F2,KEYCODE_F3,KEYCODE_F4,KEYCODE_F5,KEYCODE_F6,
    KEYCODE_F7,KEYCODE_F8,KEYCODE_F9,KEYCODE_F10,KEYCODE_F11,KEYCODE_F12,
    KEYCODE_CTRL_LEFT,KEYCODE_SHIFT_LEFT,KEYCODE_VOLUME_UP,KEYCODE_VOLUME_DOWN};
static const char *names[16]={"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "CTRL_LEFT","SHIFT_LEFT","VOLUME_UP","VOLUME_DOWN"};

static void observed(const Input_KeyEvent *event)
{
    int code=get_code(event),action=get_action(event);
    printf("INPUT_MONITOR_EVENT code=%d action=%d\n",code,action);
    for(unsigned i=0;i<16;++i)if(code==codes[i]){
        if(action==KEY_ACTION_DOWN)atomic_fetch_add(&down[i],1);
        if(action==KEY_ACTION_UP)atomic_fetch_add(&up[i],1);
    }
}

int main(int argc,char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):15;
    if(seconds<5||seconds>60)return 2;
    alarm(seconds+20);setbuf(stdout,NULL);
    printf("INPUT_MONITOR_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    FILE *status=fopen("/proc/self/status","r");
    if(status){char line[256];while(fgets(line,sizeof(line),status))if(!strncmp(line,"CapEff:",7))fputs(line,stdout);fclose(status);}
    void *lib=dlopen("/system/lib64/ndk/libohinput.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("INPUT_MONITOR_DLOPEN_FAILED %s\n",dlerror());return 2;}
    get_code=dlsym(lib,"OH_Input_GetKeyEventKeyCode");get_action=dlsym(lib,"OH_Input_GetKeyEventAction");
    Input_Result (*add)(Input_KeyEventCallback)=dlsym(lib,"OH_Input_AddKeyEventMonitor");
    Input_Result (*remove)(Input_KeyEventCallback)=dlsym(lib,"OH_Input_RemoveKeyEventMonitor");
    if(!get_code||!get_action||!add||!remove){puts("INPUT_MONITOR_API_UNAVAILABLE");return 2;}
    int added=add(observed);printf("INPUT_MONITOR_ADD result=%d\n",added);
    if(added)return 2;
    puts("INPUT_MONITOR_READY");
    for(unsigned i=0;i<seconds*10;++i)usleep(100000);
    int removed=remove(observed);usleep(200000);printf("INPUT_MONITOR_REMOVE result=%d\n",removed);
    int failed=removed!=0;
    for(unsigned i=0;i<16;++i){
        unsigned d=atomic_load(&down[i]),u=atomic_load(&up[i]);
        printf("INPUT_MONITOR_KEY name=%s code=%d down=%u up=%u required=%d\n",names[i],codes[i],d,u,i<14);
        if(i<14&&(!d||d!=u))failed=1;
    }
    printf("INPUT_MONITOR_RESULT required_function_and_modifier_keys_pass=%d media_keys_are_separate_observations=1\n",!failed);
    return failed?2:0;
}
