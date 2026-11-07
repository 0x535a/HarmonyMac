/* Original Ark+NAPI+ACE globals, with a diagnostic re-run of the real state ABC. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
template<class T>T sym(void *lib,const char *name)
{auto p=reinterpret_cast<T>(dlsym(lib,name));if(!p){printf("ARKUI_SYMBOL_MISSING %s\n",name);exit(2);}return p;}
int main(int argc,char **argv)
{
    setbuf(stdout,nullptr);alarm(25);if(argc!=2)return 2;
    auto ark=dlopen("/system/lib64/platformsdk/libark_jsruntime.so",RTLD_NOW|RTLD_LOCAL);
    auto napi=dlopen("/system/lib64/platformsdk/libace_napi.z.so",RTLD_NOW|RTLD_LOCAL);
    auto ace=dlopen("/system/lib64/platformsdk/libace_compatible.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!ark||!napi||!ace){puts(dlerror());return 2;}
    auto opt=sym<void(*)(void*)>(ark,"_ZN5panda10ecmascript16JSRuntimeOptionsC1Ev");
    auto create=sym<void*(*)(const void*)>(ark,"_ZN5panda6JSNApi12CreateEcmaVMERKNS_10ecmascript16JSRuntimeOptionsE");
    auto ctor=sym<void(*)(void*,void*,void*,bool)>(napi,"_ZN15ArkNativeEngineC1EPN5panda10ecmascript6EcmaVMEPvb");
    auto preload=sym<void(*)(void*)>(ace,"OHOS_ACE_PreloadAceModule");
    auto execute=sym<bool(*)(void*,const uint8_t*,int,const std::string&,const std::string&,bool,void*)>(ark,
      "_ZN5panda6JSNApi7ExecuteEPNS_10ecmascript6EcmaVMEPKhiRKNSt3__h12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEESE_bPv");
    auto absolute=sym<bool(*)(const void*,const std::string&,const std::string&,bool,const int&)>(ark,
      "_ZN5panda6JSNApi22ExecuteForAbsolutePathEPKNS_10ecmascript6EcmaVMERKNSt3__h12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEESD_bRKNS1_12ExecuteTypesE");
    auto global=sym<uintptr_t(*)(const void*)>(ark,"_ZN5panda6JSNApi15GetGlobalObjectEPKNS_10ecmascript6EcmaVME");
    auto get=sym<uintptr_t(*)(const void*,const void*,const char*)>(ark,"_ZN5panda9ObjectRef3GetEPKNS_10ecmascript6EcmaVMEPKc");
    auto integer=sym<int(*)(const void*,const void*)>(ark,"_ZN5panda10JSValueRef10Int32ValueEPKNS_10ecmascript6EcmaVME");
    auto scope_ctor=sym<void(*)(void*,const void*)>(ark,"_ZN5panda10LocalScopeC1EPKNS_10ecmascript6EcmaVME");
    auto print_exception=sym<void(*)(const void*)>(ark,"_ZN5panda6JSNApi18PrintExceptionInfoEPKNS_10ecmascript6EcmaVME");
    void *options=calloc(1,65536),*engine=calloc(1,65536);opt(options);void *vm=create(options);if(!vm)return 2;
    ctor(engine,vm,nullptr,false);alignas(16) unsigned char scope[256]={};scope_ctor(scope,vm);
    preload(engine);print_exception(vm);
    std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
    if(bytes.empty()||bytes.size()>1024*1024)return 2;
    auto query=[&](){bool ok=execute(vm,bytes.data(),bytes.size(),"arkui_globals","_GLOBAL::func_main_0",false,nullptr);
      uintptr_t obj=global(vm),result=obj?get(reinterpret_cast<void*>(obj),vm,"__auroraArkProbe"):0;
      return ok&&result?integer(reinterpret_cast<void*>(result),vm):-1;};
    int before=query();printf("ARKUI_GLOBALS_BEFORE=%d\n",before);
    int type=0;bool loaded=absolute(vm,"/etc/abc/framework/stateMgmt.abc","_GLOBAL::func_main_0",false,type);
    printf("ARKUI_STATE_FILE_EXECUTE=%d\n",loaded);print_exception(vm);
    int after=query();printf("ARKUI_GLOBALS_AFTER=%d\n",after);
    if(after==31){puts("ARKUI_GLOBALS_FULL_PASS");_Exit(0);}_Exit(7);
}
