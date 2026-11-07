/* Test-only stock RS client scene. Public OHOS 6 config layout, platform C++ ABI.
 * No private RS object storage or code patches are used. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
struct Config {
    std::string name="Aurora RS test";
    void *additional=nullptr;
    bool texture=false;
    uint64_t surface=0;
    bool sync=true;
    uint8_t window_type=0;
    std::shared_ptr<void> context;
};
struct DisplayConfig {
    uint64_t screen=0;
    bool mirrored=false;
    uint64_t mirror_node=0;
    bool sync=false;
    uint32_t rotation=4;
};
static_assert(offsetof(Config,texture)==32 && offsetof(Config,surface)==40 &&
              offsetof(Config,sync)==48 && offsetof(Config,window_type)==49);
template<class T> T symbol(void *lib,const char *name)
{ auto p=reinterpret_cast<T>(dlsym(lib,name));if (!p) {printf("RS_SCENE_MISSING %s\n",name);exit(2);}return p; }
int main()
{
    setbuf(stdout,nullptr);alarm(20);
    unsigned hold=3;
    if (const char *value=std::getenv("HMOS_SCENE_HOLD_SECONDS")) {
        unsigned requested=std::strtoul(value,nullptr,10);
        if (requested>=3 && requested<=8) hold=requested;
    }
    void *lib=dlopen("/system/lib64/librender_service_client.z.so",RTLD_NOW|RTLD_LOCAL);
    if (!lib) {puts(dlerror());return 2;}
    auto create=symbol<std::shared_ptr<void> (*)(const Config&,bool,std::shared_ptr<void>)>(lib,
        "_ZN4OHOS5Rosen13RSSurfaceNode6CreateERKNS0_19RSSurfaceNodeConfigEbNSt3__h10shared_ptrINS0_11RSUIContextEEE");
    auto display_create=symbol<std::shared_ptr<void> (*)(const DisplayConfig&,std::shared_ptr<void>)>(lib,
        "_ZN4OHOS5Rosen13RSDisplayNode6CreateERKNS0_19RSDisplayNodeConfigENSt3__h10shared_ptrINS0_11RSUIContextEEE");
    auto display_add=symbol<void (*)(void *)>(lib,"_ZN4OHOS5Rosen13RSDisplayNode20AddDisplayNodeToTreeEv");
    auto add_child=symbol<void (*)(void *,std::shared_ptr<void>,int)>(lib,"_ZN4OHOS5Rosen6RSNode8AddChildENSt3__h10shared_ptrIS1_EEi");
    auto instance=symbol<void *(*)()>(lib,"_ZN4OHOS5Rosen12RSInterfaces11GetInstanceEv");
    auto power=symbol<void (*)(void *,uint64_t,uint32_t)>(lib,"_ZN4OHOS5Rosen12RSInterfaces20SetScreenPowerStatusEmNS0_17ScreenPowerStatusE");
    auto mode=symbol<void (*)(void *,uint64_t,uint32_t)>(lib,"_ZN4OHOS5Rosen12RSInterfaces19SetScreenActiveModeEmj");
    auto bounds=symbol<void (*)(void *,float,float,float,float)>(lib,"_ZN4OHOS5Rosen6RSNode9SetBoundsEffff");
    auto frame=symbol<void (*)(void *,float,float,float,float)>(lib,"_ZN4OHOS5Rosen6RSNode8SetFrameEffff");
    auto color=symbol<void (*)(void *,uint32_t)>(lib,"_ZN4OHOS5Rosen6RSNode18SetBackgroundColorEj");
    auto attach=symbol<void (*)(void *,uint64_t)>(lib,"_ZN4OHOS5Rosen13RSSurfaceNode15AttachToDisplayEm");
    auto detach=symbol<void (*)(void *,uint64_t)>(lib,"_ZN4OHOS5Rosen13RSSurfaceNode15DetachToDisplayEm");
    auto flush=symbol<void (*)()>(lib,"_ZN4OHOS5Rosen13RSTransaction24FlushImplicitTransactionEv");
    power(instance(),0,0);mode(instance(),0,0);
    DisplayConfig display_config;auto display=display_create(display_config,nullptr);
    if (!display) {puts("RS_SCENE_DISPLAY_FAILED");return 2;}
    display_add(display.get());
    Config config;auto node=create(config,true,nullptr);
    if (!node) {puts("RS_SCENE_CREATE_FAILED");return 2;}
    bounds(node.get(),0,0,1280,800);frame(node.get(),0,0,1280,800);
    add_child(display.get(),node,-1);
    color(node.get(),0xffcc2244);attach(node.get(),0);flush();
    puts("RS_SCENE_RED_SUBMITTED");sleep(hold);
    color(node.get(),0xff2255cc);flush();puts("RS_SCENE_BLUE_SUBMITTED");sleep(hold);
    detach(node.get(),0);flush();node.reset();
    puts("RS_SCENE_COMMANDS_COMPLETE");
    return 0;
}
