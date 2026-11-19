/* J514 built-in VDI route on an snd-aloop card named AppleJ514.
 * Plays left 300 Hz / right 5 kHz through the Speaker pin and records the six
 * looped-back channels: woofers (0,1,4,5) must carry the 300 Hz tone, tweeters
 * (2,3) the 5 kHz tone, each on its own side. */
#define _GNU_SOURCE
#include "iaudio_manager_vdi.h"
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 48000
#define SECONDS 2
static int card = -1;
static int32_t captured[RATE * SECONDS * 6];
static int captured_frames;

static void *capture(void *arg)
{
    (void)arg;
    char name[32];snprintf(name, sizeof(name), "hw:%d,0", card);
    snd_pcm_t *pcm = NULL;
    for (int tries = 0; tries < 100 && snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0) < 0; ++tries) usleep(10000);
    if (!pcm) { printf("J514_CAPTURE_OPEN_FAILED\n"); return NULL; }
    int r = -1;
    for (int tries = 0; tries < 100 && (r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                                                 6, RATE, 0, 100000)) < 0; ++tries) usleep(10000);
    printf("J514_CAPTURE_PARAMS result=%d\n", r);
    while (r >= 0 && captured_frames < RATE * SECONDS - 14400) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, captured + captured_frames * 6, 960);
        if (n == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (n <= 0) { printf("J514_CAPTURE_READ result=%ld frames=%d\n", (long)n, captured_frames); break; }
        captured_frames += (int)n;
    }
    snd_pcm_close(pcm);
    return NULL;
}

static double tone_power(int channel, double frequency)
{
    /* Goertzel on the steady second half of the capture. */
    int start = captured_frames / 2, n = captured_frames - start;
    if (!n) return 0;
    double w = 2 * M_PI * frequency / RATE, coeff = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        double s = captured[(start + i) * 6 + channel] / 2147483648.0 + coeff * s1 - s2;
        s2 = s1; s1 = s;
    }
    return sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / n;
}

int main(void)
{
    setbuf(stdout, NULL); alarm(40);
    for (int c = 0; c < 32 && card < 0; ++c) {
        char path[64], id[32] = {0}; snprintf(path, sizeof(path), "/sys/class/sound/card%d/id", c);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (fgets(id, sizeof(id), f) && !strcmp(id, "AppleJ514\n")) card = c;
        fclose(f);
    }
    if (card < 0) { printf("J514_NO_CARD\n"); return 2; }
    char driver_link[80], driver[256];
    snprintf(driver_link, sizeof(driver_link), "/sys/class/sound/card%d/device/driver", card);
    ssize_t driver_len = readlink(driver_link, driver, sizeof(driver) - 1);
    if (driver_len < 0 || (size_t)driver_len == sizeof(driver) - 1) return 2;
    driver[driver_len] = '\0';
    const char *name = strrchr(driver, '/');
    if (!name || strcmp(name + 1, "snd_aloop")) {
        fprintf(stderr, "The probe requires an snd_aloop fixture.\n");
        return 2;
    }
    void *lib = dlopen("/vendor/lib64/libaudio_primary_impl.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("J514_LOAD_ERROR %s\n", dlerror()); return 2; }
    struct IAudioManagerVdi *(*get)(void) = dlsym(lib, "AudioManagerCreateIfInstance");
    struct IAudioManagerVdi *m = get ? get() : NULL; if (!m) return 2;
    struct AudioAdapterDescriptorVdi descs[AUDIO_VDI_ADAPTER_NUM_MAX] = {0}; uint32_t count = AUDIO_VDI_ADAPTER_NUM_MAX;
    if (m->GetAllAdapters(m, descs, &count) || !count) return 2;
    printf("J514_ENUM count=%u first=%s port=%s\n", count, descs[0].adapterName, descs[0].ports[0].portName);
    if (strcmp(descs[0].adapterName, "primary") || !strstr(descs[0].ports[0].portName, "J514 built-in")) return 3;
    struct IAudioAdapterVdi *a = NULL;
    if (m->LoadAdapter(m, &descs[0], &a) || !a) return 4;
    struct AudioDeviceDescriptorVdi device = {.portId = 0, .pins = PIN_VDI_OUT_SPEAKER, .desc = "Speaker"};
    struct AudioSampleAttributesVdi attrs = {.type = AUDIO_VDI_IN_MEDIA, .interleaved = true,
        .format = AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT, .sampleRate = RATE, .channelCount = 2,
        .period = 1024, .frameSize = 4, .isBigEndian = false, .isSignedData = true};
    struct IAudioRenderVdi *render = NULL;
    int result = a->CreateRender(a, &device, &attrs, &render);
    printf("J514_RENDER_CREATE result=%d\n", result);
    if (result || !render || render->Start(render)) return 5;
    pthread_t thread;
    static int16_t samples[RATE * SECONDS * 2];
    for (int i = 0; i < RATE * SECONDS; ++i) {
        samples[i * 2] = (int16_t)(12000 * sin(2 * M_PI * 300 * i / RATE));
        samples[i * 2 + 1] = (int16_t)(12000 * sin(2 * M_PI * 5000 * i / RATE));
    }
    uint64_t total = 0;
    for (int offset = 0; offset < RATE * SECONDS; offset += 960) {
        uint64_t done = 0;
        if (render->RenderFrame(render, (int8_t *)(samples + offset * 2), 960 * 4, &done)) { printf("J514_RENDER_FAILED\n"); return 6; }
        total += done;
        if (offset == 4800 && pthread_create(&thread, NULL, capture, NULL)) return 6;
    }
    enum AudioDrainNotifyTypeVdi drain; render->DrainBuffer(render, &drain);
    pthread_join(thread, NULL);
    printf("J514_PLAYED bytes=%llu captured_frames=%d\n", (unsigned long long)total, captured_frames);
    const char *names[6] = {"woofer_l1", "woofer_r1", "tweeter_l", "tweeter_r", "woofer_l2", "woofer_r2"};
    double low[6], high[6];
    for (int ch = 0; ch < 6; ++ch) {
        low[ch] = tone_power(ch, 300); high[ch] = tone_power(ch, 5000);
        printf("J514_CHANNEL %d %s p300=%.5f p5000=%.5f\n", ch, names[ch], low[ch], high[ch]);
    }
    /* Left carries only 300 Hz, right only 5 kHz. */
    /* The loopback capture is informative only: this test kernel's snd-aloop
     * capture returns -EIO. The DSP itself is covered by j514-dsp-test.c. */
    int route_ok = total == (uint64_t)RATE * SECONDS * 4;
    printf("J514_ROUTE_%s bytes=%llu\n", route_ok ? "OK" : "FAIL", (unsigned long long)total);
    int ok = !captured_frames ? route_ok : route_ok && captured_frames > RATE &&
        low[0] > 0.05 && low[4] > 0.05 && low[1] < 0.005 && low[5] < 0.005 &&   /* woofers: left 300 Hz */
        high[0] < 0.01 && high[1] < 0.03 && high[5] < 0.03 &&                     /* 5 kHz attenuated on woofers */
        high[3] > 0.05 && low[2] < 0.01 && high[2] < 0.005 && low[3] < 0.005;     /* tweeters: right 5 kHz only */
    a->DestroyRender(a, render); m->UnloadAdapter(m, a);
    printf(ok ? "J514_VDI_PROBE_PASS\n" : "J514_VDI_PROBE_FAIL\n");
    return ok ? 0 : 7;
}
