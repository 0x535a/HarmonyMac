/* SPDX-License-Identifier: GPL-3.0-only */
/* Verify the same FaceTime HD V4L2 capture path exposed to Debian. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct mapping {
    void *address;
    size_t length;
    int dma_fd;
};

static int control(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int virtual_platform(void)
{
    char compatible[32] = {0};
    FILE *file = fopen("/proc/device-tree/compatible", "rb");
    if (!file) return 0;
    size_t length = fread(compatible, 1, sizeof(compatible), file);
    fclose(file);
    return length >= sizeof("linux,dummy-virt") &&
        !memcmp(compatible, "linux,dummy-virt", sizeof("linux,dummy-virt"));
}

static int open_camera(const char *device, struct v4l2_capability *caps, int vivid)
{
    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (control(fd, VIDIOC_QUERYCAP, caps)) {
        close(fd);
        return -1;
    }
    int matches = vivid ? !memcmp(caps->driver, "vivid", sizeof("vivid")) :
        !memcmp(caps->card, "FaceTime HD Camera", sizeof("FaceTime HD Camera"));
    if (!matches) {
        close(fd);
        return -1;
    }
    return fd;
}

static int sample(int fd, const struct v4l2_capability *caps, const char *label, int dma_buf)
{
    uint32_t flags = caps->capabilities & V4L2_CAP_DEVICE_CAPS ? caps->device_caps : caps->capabilities;
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!(flags & V4L2_CAP_VIDEO_CAPTURE) || !(flags & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s single-plane streaming unavailable: caps=0x%x\n", label, flags);
        return 1;
    }
    struct v4l2_format format = {.type = type};
    format.fmt.pix.width = 1280;
    format.fmt.pix.height = 720;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (control(fd, VIDIOC_S_FMT, &format) ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
        !format.fmt.pix.width || !format.fmt.pix.height ||
        format.fmt.pix.bytesperline < format.fmt.pix.width ||
        !format.fmt.pix.sizeimage) {
        fprintf(stderr, "%s cannot negotiate NV12 capture\n", label);
        return 1;
    }
    uint32_t memory = dma_buf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    struct v4l2_requestbuffers request = {.count = 4, .type = type, .memory = memory};
    if (control(fd, VIDIOC_REQBUFS, &request)) {
        fprintf(stderr, "%s VIDIOC_REQBUFS: %s\n", label, strerror(errno));
        return 1;
    }
    if (request.count < 2 || request.count > 4) {
        fprintf(stderr, "%s unexpected queue size %u\n", label, request.count);
        return 1;
    }
    struct mapping buffers[4] = {0};
    for (uint32_t i = 0; i < request.count; ++i) buffers[i].dma_fd = -1;
    int result = 1;
    int streaming = 0;
    const char *stage = NULL;
    int heap = -1;
    printf("%s_FORMAT NV12 %ux%u stride=%u memory=%s\n", label, format.fmt.pix.width,
           format.fmt.pix.height, format.fmt.pix.bytesperline, dma_buf ? "dmabuf" : "mmap");
    if (dma_buf) {
        stage = "open DMA heap";
        heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (heap < 0) goto cleanup;
    }
    for (uint32_t i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer = {.type = type, .memory = memory, .index = i};
        int mapping_fd = fd;
        off_t offset = 0;
        if (dma_buf) {
            buffer.length = format.fmt.pix.sizeimage;
            struct dma_heap_allocation_data allocation = {
                .len = buffer.length, .fd_flags = O_RDWR | O_CLOEXEC,
            };
            stage = "allocate DMA buffer";
            if (control(heap, DMA_HEAP_IOCTL_ALLOC, &allocation)) goto cleanup;
            mapping_fd = buffers[i].dma_fd = (int)allocation.fd;
            buffer.m.fd = mapping_fd;
        } else {
            stage = "query buffer";
            if (control(fd, VIDIOC_QUERYBUF, &buffer)) goto cleanup;
            offset = buffer.m.offset;
        }
        stage = "map buffer";
        void *address = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, offset);
        if (address == MAP_FAILED) {
            int error = errno;
            fprintf(stderr, "%s mmap offset=%jd length=%u page=%ld\n",
                    label, (intmax_t)offset, buffer.length, sysconf(_SC_PAGESIZE));
            errno = error;
            goto cleanup;
        }
        buffers[i].address = address;
        buffers[i].length = buffer.length;
        stage = "queue buffer";
        if (control(fd, VIDIOC_QBUF, &buffer)) goto cleanup;
    }
    stage = "stream on";
    if (control(fd, VIDIOC_STREAMON, &type)) goto cleanup;
    streaming = 1;
    uint32_t previous_sequence = 0;
    for (unsigned frame = 0; frame < 16; ++frame) {
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        int ready;
        stage = "poll frame";
        do {
            ready = poll(&poll_fd, 1, 2000);
        } while (ready < 0 && errno == EINTR);
        if (ready == 0) errno = ETIMEDOUT;
        if (ready <= 0 || !(poll_fd.revents & POLLIN)) goto cleanup;
        stage = "dequeue/validate frame";
        struct v4l2_buffer buffer = {.type = type, .memory = memory};
        if (control(fd, VIDIOC_DQBUF, &buffer)) goto cleanup;
        if (buffer.index >= request.count || !buffer.bytesused ||
            buffer.bytesused > buffers[buffer.index].length ||
            (buffer.flags & V4L2_BUF_FLAG_ERROR) ||
            (frame && buffer.sequence <= previous_sequence)) {
            errno = EIO;
            goto cleanup;
        }
        previous_sequence = buffer.sequence;
        struct mapping *mapping = &buffers[buffer.index];
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        stage = "begin CPU access";
        if (dma_buf && control(mapping->dma_fd, DMA_BUF_IOCTL_SYNC, &sync)) goto cleanup;
        const unsigned char *pixels = mapping->address;
        uint32_t signature = UINT32_C(2166136261);
        for (uint32_t i = 0; i < buffer.bytesused; i += 257)
            signature = (signature ^ pixels[i]) * UINT32_C(16777619);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        stage = "end CPU access";
        if (dma_buf && control(mapping->dma_fd, DMA_BUF_IOCTL_SYNC, &sync)) goto cleanup;
        printf("%s_FRAME seq=%u bytes=%u sampled_hash=%08" PRIx32 "\n",
               label, buffer.sequence, buffer.bytesused, signature);
        stage = "requeue frame";
        if (dma_buf) {
            buffer.m.fd = mapping->dma_fd;
            buffer.length = mapping->length;
        }
        if (control(fd, VIDIOC_QBUF, &buffer)) goto cleanup;
    }
    result = 0;

cleanup:
    if (result) fprintf(stderr, "%s %s: %s\n", label, stage, strerror(errno));
    if (streaming && control(fd, VIDIOC_STREAMOFF, &type)) {
        fprintf(stderr, "%s stream off: %s\n", label, strerror(errno));
        result = 1;
    }
    for (uint32_t i = 0; i < request.count; ++i) {
        if (buffers[i].length) munmap(buffers[i].address, buffers[i].length);
        if (buffers[i].dma_fd >= 0) close(buffers[i].dma_fd);
    }
    if (heap >= 0) close(heap);
    if (!result) printf("%s_CAPTURE_PASS frames=16\n", label);
    return result;
}

int main(int argc, char **argv)
{
    int vivid_dma_buf = argc == 3 && !strcmp(argv[1], "--virtual-vivid");
    int vivid = vivid_dma_buf || (argc == 3 && !strcmp(argv[1], "--virtual-vivid-mmap"));
    int dma_buf = vivid_dma_buf || (argc == 3 && !strcmp(argv[1], "--dma-buf"));
    if ((argc > 2 && !vivid && !dma_buf) || (vivid && !virtual_platform())) {
        fprintf(stderr, "usage: %s [/dev/videoN] | --dma-buf /dev/videoN | --virtual-vivid /dev/videoN | --virtual-vivid-mmap /dev/videoN\n", argv[0]);
        return 2;
    }
    struct v4l2_capability caps = {0};
    int fd = -1;
    if (argc >= 2) {
        fd = open_camera(argv[argc - 1], &caps, vivid);
    } else {
        for (unsigned i = 0; i < 64 && fd < 0; ++i) {
            char device[24];
            snprintf(device, sizeof(device), "/dev/video%u", i);
            fd = open_camera(device, &caps, 0);
        }
    }
    if (fd < 0) {
        fputs(vivid ? "VIVID_ABSENT no virtual capture device found\n" :
              "APPLE_ISP_ABSENT no FaceTime HD Camera found\n", stderr);
        return 2;
    }
    int status = sample(fd, &caps, vivid ? "VIVID" : "APPLE_ISP", dma_buf);
    close(fd);
    return status;
}
