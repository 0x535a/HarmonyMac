/* Exercise real OHOS LLVM MCJIT code generation and executable memory. */
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    LLVMLinkInMCJIT();
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        puts("LLVM_JIT_TARGET_INIT_FAILED");
        return 1;
    }
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("aurora-ohos-jit", context);
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(module, triple);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef signature = LLVMFunctionType(i32, &i32, 1, 0);
    LLVMValueRef function = LLVMAddFunction(module, "aurora_jit_compute", signature);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, function, "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef product = LLVMBuildMul(builder, LLVMGetParam(function, 0), LLVMConstInt(i32, 3, 0), "product");
    LLVMBuildRet(builder, LLVMBuildAdd(builder, product, LLVMConstInt(i32, 7, 0), "sum"));
    LLVMDisposeBuilder(builder);
    struct LLVMMCJITCompilerOptions options;
    LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
    options.OptLevel = 2;
    LLVMExecutionEngineRef engine;
    char *error = NULL;
    if (LLVMCreateMCJITCompilerForModule(&engine, module, &options, sizeof(options), &error)) {
        printf("LLVM_JIT_CREATE_FAILED: %s\n", error ? error : "unknown");
        if (error) LLVMDisposeMessage(error);
        LLVMDisposeModule(module);
        LLVMDisposeMessage(triple);
        LLVMContextDispose(context);
        return 2;
    }
    uint64_t address = LLVMGetFunctionAddress(engine, "aurora_jit_compute");
    if (!address) {
        puts("LLVM_JIT_ADDRESS_FAILED");
        return 3;
    }
    int (*compute)(int) = (int (*)(int))(uintptr_t)address;
    for (int i = 0; i < 1000; i++) {
        int actual = compute(i - 500);
        if (actual != (i - 500) * 3 + 7) {
            printf("LLVM_JIT_VALUE_FAILED input=%d actual=%d\n", i - 500, actual);
            return 4;
        }
    }
    printf("LLVM_JIT_PASS calls=1000 triple=%s userspace_page_size=%ld\n", triple, sysconf(_SC_PAGESIZE));
    LLVMDisposeExecutionEngine(engine);
    LLVMDisposeMessage(triple);
    LLVMContextDispose(context);
    return 0;
}
