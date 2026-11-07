/* Test-only adapter for the fixed ISO's original Ark C++ exports.
 * The original constructors own object initialization. Opaque storage avoids
 * substituting SDK std::__n1 objects for the platform's std::__h objects.
 * Do not install this diagnostic as a replacement system service.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct short_string { unsigned char size_and_flag; char bytes[23]; };
_Static_assert(sizeof(struct short_string) == 24, "platform short string ABI");

static void *symbol(void *library, const char *name)
{
    void *result = dlsym(library, name);
    if (!result) {
        fprintf(stderr, "ARK_PROBE_MISSING %s: %s\n", name, dlerror());
        exit(2);
    }
    return result;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    alarm(30);
    if (argc != 2) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) return 3;
    long length = ftell(input);
    if (length <= 0 || length > 1024 * 1024) return 3;
    rewind(input);
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, input) != (size_t)length) return 3;
    fclose(input);
    puts("ARK_PROBE_BEGIN");
    void *library = dlopen("/system/lib64/platformsdk/libark_jsruntime.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "ARK_PROBE_LOAD %s\n", dlerror());
        return 4;
    }
    void (*options_ctor)(void *) = symbol(library, "_ZN5panda10ecmascript16JSRuntimeOptionsC1Ev");
    void *(*create)(const void *) = symbol(library, "_ZN5panda6JSNApi12CreateEcmaVMERKNS_10ecmascript16JSRuntimeOptionsE");
    void (*destroy)(void *) = symbol(library, "_ZN5panda6JSNApi11DestroyJSVMEPNS_10ecmascript6EcmaVME");
    void (*scope_ctor)(void *, const void *) = symbol(library, "_ZN5panda10LocalScopeC1EPKNS_10ecmascript6EcmaVME");
    void (*scope_dtor)(void *) = symbol(library, "_ZN5panda10LocalScopeD1Ev");
    bool (*execute)(void *, const unsigned char *, int, const void *, bool) = symbol(library,
        "_ZN5panda6JSNApi19ExecuteModuleBufferEPNS_10ecmascript6EcmaVMEPKhiRKNSt3__h12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEEb");
    uintptr_t (*global)(const void *) = symbol(library, "_ZN5panda6JSNApi15GetGlobalObjectEPKNS_10ecmascript6EcmaVME");
    uintptr_t (*get)(const void *, const void *, const char *) = symbol(library,
        "_ZN5panda9ObjectRef3GetEPKNS_10ecmascript6EcmaVMEPKc");
    int (*integer)(const void *, const void *) = symbol(library,
        "_ZN5panda10JSValueRef10Int32ValueEPKNS_10ecmascript6EcmaVME");
    void *options = calloc(1, 65536);
    if (!options) return 5;
    options_ctor(options);
    puts("ARK_PROBE_OPTIONS_READY");
    void *vm = create(options);
    if (!vm) {
        puts("ARK_PROBE_NO_VM");
        return 6;
    }
    puts("ARK_PROBE_VM_READY");
    _Alignas(16) unsigned char scope[256] = {0};
    scope_ctor(scope, vm);
    puts("ARK_PROBE_SCOPE_READY");
    if (getenv("HMOS_PRELOAD_ACE")) {
        /* ACE expects NativeEngine*, not EcmaVM*. Use the original NAPI
         * constructor; this short-lived diagnostic retains it until exit. */
        void *napi=dlopen("/system/lib64/platformsdk/libace_napi.z.so",RTLD_NOW|RTLD_LOCAL);
        if (!napi) {fprintf(stderr,"ARKUI_PROBE_NAPI %s\n",dlerror());return 8;}
        void (*engine_ctor)(void *,void *,void *,bool)=symbol(napi,
            "_ZN15ArkNativeEngineC1EPN5panda10ecmascript6EcmaVMEPvb");
        void *engine=calloc(1,65536);
        if (!engine) return 8;
        engine_ctor(engine,vm,NULL,false);
        void *ace=dlopen("/system/lib64/platformsdk/libace_compatible.z.so",RTLD_NOW|RTLD_LOCAL);
        if (!ace) {fprintf(stderr,"ARKUI_PROBE_LOAD %s\n",dlerror());return 8;}
        void (*preload)(void *)=symbol(ace,"OHOS_ACE_PreloadAceModule");
        preload(engine);
        puts("ARKUI_PROBE_PRELOAD_RETURNED");
    }
    struct short_string filename = {0};
    strcpy(filename.bytes, "aurora_probe");
    /* The fixed Ark binary tests bit 0 in byte 0 and reads long data at +16:
     * this platform uses the regular libc++ short-string layout. */
    filename.size_and_flag = (unsigned char)(strlen(filename.bytes) << 1);
    bool ok;
    if (getenv("HMOS_SCRIPT")) {
        bool (*script)(void *,const unsigned char *,int,const void *,const void *,bool,void *)=symbol(library,
            "_ZN5panda6JSNApi7ExecuteEPNS_10ecmascript6EcmaVMEPKhiRKNSt3__h12basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEESE_bPv");
        struct short_string entry={0};strcpy(entry.bytes,"_GLOBAL::func_main_0");
        entry.size_and_flag=(unsigned char)(strlen(entry.bytes)<<1);
        ok=script(vm,bytes,(int)length,&filename,&entry,false,NULL);
    } else {
        ok=execute(vm, bytes, (int)length, &filename, false);
    }
    printf("ARK_PROBE_BYTECODE_RETURN=%d\n", ok);
    int value = -1;
    if (ok) {
        uintptr_t object = global(vm);
        printf("ARK_PROBE_GLOBAL=%p\n", (void *)object);
        if (object) {
            uintptr_t answer = get((void *)object, vm, "__auroraArkProbe");
            if (answer) value = integer((void *)answer, vm);
        }
    }
    printf("ARK_PROBE_EXECUTE=%d RESULT=%d\n", ok, value);
    if (getenv("HMOS_PRELOAD_ACE")) {
        int expected=getenv("HMOS_EXPECTED_RESULT") ? atoi(getenv("HMOS_EXPECTED_RESULT")) : 31;
        if (ok && value==expected) {puts("ARKUI_ORIGINAL_GLOBALS_PASS");_Exit(0);}
        _Exit(7);
    }
    scope_dtor(scope);
    destroy(vm);
    free(options);
    free(bytes);
    int expected=getenv("HMOS_EXPECTED_RESULT") ? atoi(getenv("HMOS_EXPECTED_RESULT")) : 500500;
    if (!ok || value != expected) return 7;
    if (getenv("HMOS_PRELOAD_ACE")) {puts("ARKUI_ORIGINAL_GLOBALS_PASS");return 0;}
    puts("ARK_ORIGINAL_RUNTIME_PASS computed JavaScript sum=500500");
    return 0;
}
