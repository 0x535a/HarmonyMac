/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../../l410/dmabuf-ion/dmabuf_alloc.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void SetOwnerIdForHeapFlags(DmabufHeapBuffer *buffer, enum DmaHeapFlagOwnerId owner)
{
    if (buffer && owner >= DMA_OWNER_DEFAULT && owner < COUNT_DMA_OWNER) {
        buffer->heapFlags &= ~(3ULL << 32);
        buffer->heapFlags |= (uint64_t)owner << 32;
    }
}

int DmabufHeapOpen(const char *name)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    /* These ordinary-memory names differ across the source and target BSP.
     * Protected/secure/CPA heaps are never redirected to ordinary memory. */
    if (strcmp(name, "system") && strcmp(name, "system_heap") &&
        strcmp(name, "system-uncached") && strcmp(name, "tiny_heap")) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
}

int DmabufHeapClose(unsigned int fd)
{
    return close((int)fd);
}

int DmabufHeapBufferAlloc(unsigned int heap, DmabufHeapBuffer *buffer)
{
    if (!buffer || !buffer->size) {
        errno = EINVAL;
        return -1;
    }
    struct dma_heap_allocation_data request = {
        .len = buffer->size,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    int ret = ioctl((int)heap, DMA_HEAP_IOCTL_ALLOC, &request);
    if (!ret) buffer->fd = request.fd;
    return ret;
}

int DmabufHeapBufferFree(DmabufHeapBuffer *buffer)
{
    if (!buffer || buffer->fd == (unsigned int)-1) {
        errno = EINVAL;
        return -1;
    }
    int ret = close((int)buffer->fd);
    if (!ret) buffer->fd = (unsigned int)-1;
    return ret;
}

static int sync_buffer(unsigned int fd, DmabufHeapBufferSyncType type, uint64_t phase)
{
    if (type != DMA_BUF_HEAP_BUF_SYNC_READ && type != DMA_BUF_HEAP_BUF_SYNC_WRITE &&
        type != DMA_BUF_HEAP_BUF_SYNC_RW) {
        errno = EINVAL;
        return -1;
    }
    struct dma_buf_sync request = {.flags = phase | (uint64_t)type};
    return ioctl((int)fd, DMA_BUF_IOCTL_SYNC, &request);
}

int DmabufHeapBufferSyncStart(unsigned int fd, DmabufHeapBufferSyncType type)
{
    return sync_buffer(fd, type, DMA_BUF_SYNC_START);
}

int DmabufHeapBufferSyncEnd(unsigned int fd, DmabufHeapBufferSyncType type)
{
    return sync_buffer(fd, type, DMA_BUF_SYNC_END);
}
