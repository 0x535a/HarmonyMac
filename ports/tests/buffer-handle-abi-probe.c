#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    void *lib = dlopen("libbuffer_handle.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts(dlerror()); return 2; }
    void *(*allocate)(uint32_t, uint32_t) = dlsym(lib, "AllocateBufferHandle");
    void (*release)(void *) = dlsym(lib, "FreeBufferHandle");
    if (!allocate || !release) return 3;
    void *handle = allocate(2, 3);
    if (!handle) return 4;
    uint32_t fields[4];
    memcpy(fields, (char *)handle + 48, sizeof(fields));
    printf("BUFFER_HANDLE_ABI offsets48,52,56,60=%u,%u,%u,%u\n", fields[0], fields[1], fields[2], fields[3]);
    int size = 0;
    if (fields[0] == 2 && fields[1] == 3) size = 56;
    else if (fields[2] == 2 && fields[3] == 3) size = 64;
    printf("BUFFER_HANDLE_ABI_FIXED_SIZE=%d\n", size);
    release(handle);
    return size ? 0 : 5;
}
