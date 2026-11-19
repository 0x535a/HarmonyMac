/* SPDX-License-Identifier: GPL-3.0-only
 * Stock public MMI monitor -> bounded standard keyboard LED control.
 * Only keyboard-illumination key events enter the queue. This monitor never
 * consumes input, modifies key mappings or logs other application keys.
 */
#define _GNU_SOURCE
#include "keyboard-backlight.h"
#include <stdbool.h>
#include <multimodalinput/oh_input_manager.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/capability.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { TOGGLE, DOWN, UP, QUEUE_SIZE = 32 };
static int32_t codes[3];
static int32_t (*get_code)(const Input_KeyEvent *);
static int32_t (*get_action)(const Input_KeyEvent *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned queue[QUEUE_SIZE], first, count;
static bool handled[3], stopping;
static volatile sig_atomic_t terminate_requested;

static void terminate(int signal_number)
{ (void)signal_number; terminate_requested = 1; }

static void observed(const Input_KeyEvent *event)
{
    int code = get_code(event), action = get_action(event), key = -1;
    for (unsigned i = 0; i < 3; ++i) if (code == codes[i]) key = (int)i;
    if (key < 0 || (action != KEY_ACTION_DOWN && action != KEY_ACTION_UP)) return;
    pthread_mutex_lock(&lock);
    bool enqueue = false;
    if (action == KEY_ACTION_DOWN) {
        enqueue = key != TOGGLE || !handled[key];
        handled[key] = true;
    } else {
        // Some stock system shortcuts consume a down event before monitors.
        // Act on release only if this press was not already handled.
        enqueue = !handled[key];
        handled[key] = false;
    }
    if (!stopping && enqueue) {
        if (count < QUEUE_SIZE) {
            queue[(first + count) % QUEUE_SIZE] = (unsigned)key;
            ++count;
            pthread_cond_signal(&condition);
        } else {
            fputs("KEYBOARD_LIGHT_QUEUE_FULL\n", stderr);
        }
    }
    pthread_mutex_unlock(&lock);
}

static void *control(void *unused)
{
    (void)unused;
    unsigned remembered = 128;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        while (!stopping && !count) pthread_cond_wait(&condition, &lock);
        if (stopping) break;
        unsigned key = queue[first];
        first = (first + 1) % QUEUE_SIZE;
        --count;
        pthread_mutex_unlock(&lock);
        uint32_t before;
        if (AuroraGetKeyboardBacklight(&before)) {
            fprintf(stderr, "KEYBOARD_LIGHT_UNAVAILABLE errno=%d\n", errno);
        } else {
            unsigned target;
            if (key == UP) target = before > 239 ? 255 : before + 16;
            else if (key == DOWN) target = before < 16 ? 0 : before - 16;
            else {
                if (before) remembered = before;
                target = before ? 0 : remembered;
            }
            if (AuroraSetKeyboardBacklight(target))
                fprintf(stderr, "KEYBOARD_LIGHT_APPLY_FAILED errno=%d key=%u\n", errno, key);
            else {
                if (target) remembered = target;
                printf("KEYBOARD_LIGHT_ACTION code=%d before=%u after=%u\n", codes[key], before, target);
            }
        }
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    setbuf(stdout, NULL);
    // Root is the existing LED attribute owner. No Linux capabilities,
    // setuid helper, device-group widening or raw input access is needed.
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct __user_cap_data_struct capabilities[2] = {{0}, {0}};
    if (syscall(SYS_capset, &header, capabilities) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        perror("keyboard-light-hotkeys: cannot drop capabilities"); return 2;
    }
    printf("KEYBOARD_LIGHT_IDENTITY uid=%u gid=%u\n", getuid(), getgid());
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        while (fgets(line, sizeof(line), status)) if (!strncmp(line, "CapEff:", 7)) fputs(line, stdout);
        fclose(status);
    }
    void *mmi = dlopen("/system/lib64/platformsdk/libmmi-client.z.so", RTLD_NOW | RTLD_LOCAL);
    void *native = dlopen("/system/lib64/ndk/libohinput.so", RTLD_NOW | RTLD_LOCAL);
    if (!mmi || !native) { fprintf(stderr, "KEYBOARD_LIGHT_LIBRARY_FAILED %s\n", dlerror()); return 2; }
    const char *names[3] = {
        "_ZN4OHOS3MMI8KeyEvent23KEYCODE_KBDILLUM_TOGGLEE",
        "_ZN4OHOS3MMI8KeyEvent21KEYCODE_KBDILLUM_DOWNE",
        "_ZN4OHOS3MMI8KeyEvent19KEYCODE_KBDILLUM_UPE",
    };
    for (unsigned i = 0; i < 3; ++i) {
        const int32_t *value = dlsym(mmi, names[i]);
        if (!value || *value <= 0 || *value == 40 || *value == 41) {
            fputs("KEYBOARD_LIGHT_KEY_SYMBOL_FAILED\n", stderr); return 2;
        }
        codes[i] = *value;
    }
    if (codes[0] == codes[1] || codes[1] == codes[2] || codes[0] == codes[2]) return 2;
    get_code = dlsym(native, "OH_Input_GetKeyEventKeyCode");
    get_action = dlsym(native, "OH_Input_GetKeyEventAction");
    Input_Result (*add)(Input_KeyEventCallback) = dlsym(native, "OH_Input_AddKeyEventMonitor");
    Input_Result (*remove)(Input_KeyEventCallback) = dlsym(native, "OH_Input_RemoveKeyEventMonitor");
    if (!get_code || !get_action || !add || !remove) return 2;
    signal(SIGTERM, terminate);
    signal(SIGINT, terminate);
    int result = -1;
    for (unsigned attempt = 0; attempt < 60 && !terminate_requested; ++attempt) {
        result = add(observed);
        if (!result || result == 201) break;
        sleep(1);
    }
    if (result) { fprintf(stderr, "KEYBOARD_LIGHT_MONITOR_FAILED result=%d\n", result); return 2; }
    pthread_t worker;
    if (pthread_create(&worker, NULL, control, NULL)) { remove(observed); return 2; }
    printf("KEYBOARD_LIGHT_HOTKEYS_READY toggle=%d down=%d up=%d\n", codes[0], codes[1], codes[2]);
    while (!terminate_requested) sleep(1);
    result = remove(observed);
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    printf("KEYBOARD_LIGHT_HOTKEYS_STOPPED remove=%d\n", result);
    return result ? 2 : 0;
}
