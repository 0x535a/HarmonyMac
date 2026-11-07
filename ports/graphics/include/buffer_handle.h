/* SPDX-License-Identifier: GPL-3.0-only
 * Fixed HAD-W32 ISO ABI, verified by AllocateBufferHandle(2,3) and disassembly.
 * Do not replace with the API20 public SDK's 64-byte key-bearing structure.
 */
#ifndef INCLUDE_BUFFER_HANDLE_H
#define INCLUDE_BUFFER_HANDLE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int32_t fd;
    int32_t width;
    int32_t stride;
    int32_t height;
    int32_t size;
    int32_t format;
    uint64_t usage;
    void *virAddr;
    uint64_t phyAddr;
    uint32_t reserveFds;
    uint32_t reserveInts;
    int32_t reserve[0];
} BufferHandle;
BufferHandle *AllocateBufferHandle(uint32_t reserveFds, uint32_t reserveInts);
void FreeBufferHandle(BufferHandle *handle);
#ifdef __cplusplus
}
static_assert(sizeof(BufferHandle) == 56, "fixed platform buffer handle size");
static_assert(offsetof(BufferHandle, reserveFds) == 48, "fixed FD table offset");
#else
_Static_assert(sizeof(BufferHandle) == 56, "fixed platform buffer handle size");
#endif
#endif
