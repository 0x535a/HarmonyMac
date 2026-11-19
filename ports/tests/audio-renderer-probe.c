/* Real public OHAudio renderer -> original AudioServer -> HDI -> USB PCM.
 * Only run in the controlled VM with a WAV-only host backend. */
#define _GNU_SOURCE
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct State { _Atomic uint64_t frames; _Atomic unsigned calls,errors; };
static int32_t write_data(OH_AudioRenderer *renderer,void *opaque,void *buffer,int32_t length)
{
    (void)renderer;struct State *s=opaque;
    if(!buffer || length<=0 || length%4) { atomic_fetch_add(&s->errors,1);return -1; }
    uint64_t first=atomic_load(&s->frames);int16_t *samples=buffer;
    for(int i=0;i<length/4;++i) {
        uint64_t position=first+i;int active=position>=4800 && position<52800;
        samples[i*2]=active?((((position-4800)/24)&1)?8192:-8192):0;
        samples[i*2+1]=active?((((position-4800)/48)&1)?4096:-4096):0;
    }
    atomic_fetch_add(&s->frames,(unsigned)length/4);atomic_fetch_add(&s->calls,1);return 0;
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);alarm(40);
    int effect_none=argc==2 && !strcmp(argv[1],"--effect-none");if(argc>2 || (argc==2 && !effect_none))return 2;
    printf("OHAUDIO_IDENTITY uid=%u gid=%u\n",getuid(),getgid());
    void *lib=dlopen("/system/lib64/ndk/libohaudio.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib)lib=dlopen("/system/lib64/libohaudio.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib) { printf("OHAUDIO_LOAD_FAIL %s\n",dlerror());return 2; }
#define IMPORT(name) __typeof__(&name) name##_fn=(__typeof__(&name))dlsym(lib,#name); if(!name##_fn) { printf("OHAUDIO_MISSING %s\n",#name);return 2; }
    IMPORT(OH_AudioStreamBuilder_Create);IMPORT(OH_AudioStreamBuilder_Destroy);
    IMPORT(OH_AudioStreamBuilder_SetSamplingRate);IMPORT(OH_AudioStreamBuilder_SetChannelCount);
    IMPORT(OH_AudioStreamBuilder_SetSampleFormat);IMPORT(OH_AudioStreamBuilder_SetLatencyMode);
    IMPORT(OH_AudioStreamBuilder_SetRendererInfo);IMPORT(OH_AudioStreamBuilder_SetRendererCallback);
    IMPORT(OH_AudioStreamBuilder_GenerateRenderer);IMPORT(OH_AudioRenderer_Start);
    IMPORT(OH_AudioRenderer_Stop);IMPORT(OH_AudioRenderer_Release);IMPORT(OH_AudioRenderer_GetFramesWritten);
#undef IMPORT
    OH_AudioStreamBuilder *builder=NULL;OH_AudioRenderer *renderer=NULL;struct State state={0};
    int result=OH_AudioStreamBuilder_Create_fn(&builder,AUDIOSTREAM_TYPE_RENDERER);
    printf("OHAUDIO_CREATE result=%d builder=%d\n",result,builder!=NULL);if(result || !builder)return 2;
    OH_AudioRenderer_Callbacks callbacks={.OH_AudioRenderer_OnWriteData=write_data};
#define CONFIG(name,...) do { result=name##_fn(builder,__VA_ARGS__);printf("OHAUDIO_CONFIG %s=%d\n",#name,result);if(result)goto fail; } while(0)
    CONFIG(OH_AudioStreamBuilder_SetSamplingRate,48000);
    CONFIG(OH_AudioStreamBuilder_SetChannelCount,2);
    CONFIG(OH_AudioStreamBuilder_SetSampleFormat,AUDIOSTREAM_SAMPLE_S16LE);
    CONFIG(OH_AudioStreamBuilder_SetLatencyMode,AUDIOSTREAM_LATENCY_MODE_NORMAL);
    CONFIG(OH_AudioStreamBuilder_SetRendererInfo,AUDIOSTREAM_USAGE_MUSIC);
    CONFIG(OH_AudioStreamBuilder_SetRendererCallback,callbacks,&state);
#undef CONFIG
    result=OH_AudioStreamBuilder_GenerateRenderer_fn(builder,&renderer);
    printf("OHAUDIO_GENERATE result=%d renderer=%d\n",result,renderer!=NULL);if(result || !renderer)goto fail;
    if(effect_none) {
        __typeof__(&OH_AudioRenderer_SetEffectMode) set_effect=dlsym(lib,"OH_AudioRenderer_SetEffectMode");
        __typeof__(&OH_AudioRenderer_GetEffectMode) get_effect=dlsym(lib,"OH_AudioRenderer_GetEffectMode");
        if(!set_effect || !get_effect)goto fail;
        result=set_effect(renderer,EFFECT_NONE);OH_AudioStream_AudioEffectMode mode=EFFECT_DEFAULT;
        if(!result)result=get_effect(renderer,&mode);
        printf("OHAUDIO_EFFECT_NONE result=%d mode=%d\n",result,(int)mode);if(result || mode!=EFFECT_NONE)goto fail;
    }
    result=OH_AudioRenderer_Start_fn(renderer);printf("OHAUDIO_START result=%d\n",result);if(result)goto fail;
    for(unsigned i=0;i<1500 && atomic_load(&state.frames)<100800 && !atomic_load(&state.errors);++i)usleep(10000);
    int64_t written=-1;result=OH_AudioRenderer_GetFramesWritten_fn(renderer,&written);
    printf("OHAUDIO_WRITTEN result=%d reported=%lld callback_frames=%llu callbacks=%u errors=%u\n",result,
        (long long)written,(unsigned long long)atomic_load(&state.frames),atomic_load(&state.calls),atomic_load(&state.errors));
    if(result || written<=0 || atomic_load(&state.frames)<100800 || atomic_load(&state.errors))goto fail;
    result=OH_AudioRenderer_Stop_fn(renderer);printf("OHAUDIO_STOP result=%d\n",result);if(result)goto fail;
    result=OH_AudioRenderer_Release_fn(renderer);renderer=NULL;if(result)goto fail;
    result=OH_AudioStreamBuilder_Destroy_fn(builder);if(result)return 2;
    puts("OHAUDIO_CALLBACK_SUBMISSION_PASS independent_host_waveform_required=1");return 0;
fail:
    if(renderer) { OH_AudioRenderer_Stop_fn(renderer);OH_AudioRenderer_Release_fn(renderer); }
    OH_AudioStreamBuilder_Destroy_fn(builder);puts("OHAUDIO_PROBE_FAIL");return 2;
}
