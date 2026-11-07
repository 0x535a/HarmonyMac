/* Observe original public MMI mouse coordinates; never inject or consume events. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int (*action)(const void *),(*x)(const void *),(*y)(const void *),(*button)(const void *),(*axis)(const void *),(*window)(const void *);
static float (*value)(const void *);
static void event(const void *e)
{
    printf("POINTER action=%d x=%d y=%d button=%d axis=%d value=%g window=%d\n",
           action(e),x(e),y(e),button(e),axis(e),(double)value(e),window(e));
}
int main(int argc,char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):60;
    if (!seconds || seconds>300) return 2;
    setbuf(stdout,NULL);
    void *lib=dlopen("/system/lib64/ndk/libohinput.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){puts(dlerror());return 2;}
#define GET(v,n) v=dlsym(lib,n);if(!v)return 2
    GET(action,"OH_Input_GetMouseEventAction");
    GET(x,"OH_Input_GetMouseEventDisplayX");GET(y,"OH_Input_GetMouseEventDisplayY");
    GET(button,"OH_Input_GetMouseEventButton");GET(axis,"OH_Input_GetMouseEventAxisType");
    GET(value,"OH_Input_GetMouseEventAxisValue");GET(window,"OH_Input_GetMouseEventWindowId");
    int (*add)(void(*)(const void *)),(*remove)(void(*)(const void *));
    GET(add,"OH_Input_AddMouseEventMonitor");GET(remove,"OH_Input_RemoveMouseEventMonitor");
#undef GET
    int ret=add(event);printf("POINTER_MONITOR_READY result=%d\n",ret);if(ret)return 2;
    sleep(seconds);ret=remove(event);printf("POINTER_MONITOR_END result=%d\n",ret);return ret?2:0;
}
