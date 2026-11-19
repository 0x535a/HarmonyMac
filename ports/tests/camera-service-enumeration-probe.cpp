/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise public camera enumeration through the normal CameraService. */
#include <ohcamera/camera_manager.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
void print_address(const char *name, size_t length, uint64_t value)
{
    char line[] = "0x0000000000000000\n";
    for (int index = 17; index >= 2; --index) {
        line[index] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    (void)write(STDERR_FILENO, name, length);
    (void)write(STDERR_FILENO, line, sizeof(line) - 1);
}

void report_fault(int signal_number, siginfo_t *info, void *context)
{
    const auto &registers = static_cast<ucontext_t *>(context)->uc_mcontext;
    print_address("CAMERA_FAULT_PC=", 16, registers.pc);
    print_address("CAMERA_FAULT_LR=", 16, registers.regs[30]);
    print_address("CAMERA_FAULT_ADDRESS=", 21, reinterpret_cast<uintptr_t>(info->si_addr));
    int memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (memory >= 0) {
        uint64_t frame = registers.regs[29];
        for (unsigned i = 0; i < 12; ++i) {
            if (frame < registers.sp || frame - registers.sp > 1024 * 1024 || frame % 16) break;
            uint64_t record[2];
            if (pread(memory, record, sizeof(record), static_cast<off_t>(frame)) != sizeof(record)) break;
            print_address("CAMERA_FAULT_FRAME=", 19, record[1] & UINT64_C(0x0000ffffffffffff));
            if (record[0] <= frame) break;
            frame = record[0];
        }
        (void)close(memory);
    }
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buffer[1024];
        ssize_t count;
        while ((count = read(fd, buffer, sizeof(buffer))) > 0)
            (void)write(STDERR_FILENO, buffer, static_cast<size_t>(count));
        (void)close(fd);
    }
    // Keep the fault visible as a failing probe without re-entering the handler.
    _exit(128 + signal_number);
}

void inspect_camera(Camera_Manager *manager, const Camera_Device &device)
{
    Camera_OutputCapability *capability = nullptr;
    auto status = OH_CameraManager_GetSupportedCameraOutputCapability(manager, &device, &capability);
    printf("CAMERA_CAPABILITY status=%d preview=%u photo=%u video=%u\n",
        static_cast<int>(status), capability ? capability->previewProfilesSize : 0,
        capability ? capability->photoProfilesSize : 0, capability ? capability->videoProfilesSize : 0);
    if (status == CAMERA_OK && capability) {
        for (uint32_t n = 0; n < capability->previewProfilesSize; ++n) {
            const auto *profile = capability->previewProfiles[n];
            if (profile) printf("CAMERA_PREVIEW_PROFILE format=%d width=%u height=%u\n",
                static_cast<int>(profile->format), profile->size.width, profile->size.height);
        }
        status = OH_CameraManager_DeleteSupportedCameraOutputCapability(manager, capability);
        printf("CAMERA_CAPABILITY_RELEASE status=%d\n", static_cast<int>(status));
    }
    Camera_Input *input = nullptr;
    status = OH_CameraManager_CreateCameraInput(manager, &device, &input);
    printf("CAMERA_INPUT_CREATE status=%d\n", static_cast<int>(status));
    if (status != CAMERA_OK || !input) return;
    status = OH_CameraInput_Open(input);
    printf("CAMERA_INPUT_OPEN status=%d\n", static_cast<int>(status));
    if (status == CAMERA_OK) {
        status = OH_CameraInput_Close(input);
        printf("CAMERA_INPUT_CLOSE status=%d\n", static_cast<int>(status));
    }
    status = OH_CameraInput_Release(input);
    printf("CAMERA_INPUT_RELEASE status=%d\n", static_cast<int>(status));
}
}

int main(int argc, char **argv)
{
    bool inspect = argc == 2 && strcmp(argv[1], "--inspect-input") == 0;
    if (argc != 1 && !inspect) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    struct sigaction action {};
    action.sa_sigaction = report_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, nullptr) != 0) return 1;
    Camera_Manager *manager = nullptr;
    Camera_ErrorCode result = OH_Camera_GetCameraManager(&manager);
    printf("CAMERA_MANAGER status=%d\n", static_cast<int>(result));
    if (result != CAMERA_OK || !manager) return 1;

    Camera_Device *devices = nullptr;
    uint32_t count = 0;
    result = OH_CameraManager_GetSupportedCameras(manager, &devices, &count);
    printf("CAMERA_ENUMERATION status=%d count=%u\n", static_cast<int>(result), count);
    if (result != CAMERA_OK) return 1;
    if (inspect && devices && count) inspect_camera(manager, devices[0]);
    if (devices) {
        Camera_ErrorCode release = OH_CameraManager_DeleteSupportedCameras(manager, devices, count);
        printf("CAMERA_ENUMERATION_RELEASE status=%d\n", static_cast<int>(release));
        if (result == CAMERA_OK) result = release;
    }
    Camera_ErrorCode release = OH_Camera_DeleteCameraManager(manager);
    printf("CAMERA_MANAGER_RELEASE status=%d\n", static_cast<int>(release));
    if (result != CAMERA_OK || release != CAMERA_OK) return 1;
    return count ? 0 : 2;
}
