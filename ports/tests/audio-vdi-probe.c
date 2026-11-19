/* Real VDI playback and absence checks; WAV content is independently verified. */
#define _GNU_SOURCE
#include "iaudio_manager_vdi.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void release_descs(struct AudioAdapterDescriptorVdi *d,uint32_t n)
{ for(uint32_t i=0;i<n;++i) { for(uint32_t j=0;j<d[i].portsLen;++j)free(d[i].ports[j].portName);free(d[i].ports);free(d[i].adapterName); } }
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);alarm(35);
    int absent=argc==2 && !strcmp(argv[1],"--absent");int pcm32=argc==2 && !strcmp(argv[1],"--s32");
    int blocked=argc==2 && !strcmp(argv[1],"--blocked");
    if(argc>2 || (argc==2 && !absent && !pcm32 && !blocked))return 2;
    void *lib=dlopen("/vendor/lib64/libaudio_primary_impl.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib) { printf("VDI_LOAD_ERROR %s\n",dlerror());return 2; }
    struct IAudioManagerVdi *(*get)(void)=dlsym(lib,"AudioManagerCreateIfInstance");if(!get)return 2;
    struct IAudioManagerVdi *m=get();if(!m)return 2;
    struct AudioAdapterDescriptorVdi descs[AUDIO_VDI_ADAPTER_NUM_MAX]={0};uint32_t count=AUDIO_VDI_ADAPTER_NUM_MAX;
    int result=m->GetAllAdapters(m,descs,&count);printf("VDI_ENUM result=%d count=%u uid=%u\n",result,count,getuid());
    if(result || count>AUDIO_VDI_ADAPTER_NUM_MAX)return 2;
    if(blocked) {
        if(!count)return 2;
        for(uint32_t i=0;i<count;++i) {
            struct IAudioAdapterVdi *adapter=NULL;int load=m->LoadAdapter(m,&descs[i],&adapter);
            printf("VDI_EXPLICIT_OUTPUT_BLOCK name=%s result=%d adapter_null=%d\n",descs[i].adapterName,load,adapter==NULL);
            if(!load || adapter)return 2;
        }
        release_descs(descs,count);puts("VDI_EXPLICIT_OUTPUT_BLOCK_PASS real_usb_present=1 pcm_not_opened=1");
        return m->ReleaseAudioManagerObject(m)?2:0;
    }
    if(absent) {
        struct AudioAdapterDescriptorVdi stale={.adapterName="primary"};struct IAudioAdapterVdi *adapter=NULL;
        int load=m->LoadAdapter(m,&stale,&adapter);printf("VDI_ABSENT_LOAD result=%d adapter_null=%d\n",load,adapter==NULL);
        if(count || !load || adapter)return 2;
        puts("VDI_ABSENT_PASS");return m->ReleaseAudioManagerObject(m)?2:0;
    }
    uint32_t selected=0;
    for(uint32_t i=0;i<count;++i) { printf("VDI_LOGICAL_ADAPTER index=%u name=%s\n",i,descs[i].adapterName);if(!strcmp(descs[i].adapterName,"usb"))selected=i; }
    if(!count || descs[selected].portsLen!=1 || descs[selected].ports[0].dir!=PORT_VDI_OUT)return 2;
    struct IAudioAdapterVdi *a=NULL;result=m->LoadAdapter(m,&descs[selected],&a);
    printf("VDI_ADAPTER name=%s ports=%u load=%d\n",descs[selected].adapterName,descs[selected].portsLen,result);
    if(result || !a || a->InitAllPorts(a))return 2;
    struct AudioPortCapabilityVdi cap={0};result=a->GetPortCapability(a,&descs[selected].ports[0],&cap);
    printf("VDI_CAP result=%d type=%u formats=%u rates=0x%x channels=%u\n",result,cap.deviceType,cap.formatsLen,cap.sampleRateMasks,cap.channelCount);
    if(result || cap.deviceType!=AUDIO_VDI_USB_DEVICE || cap.channelCount!=2 || !(cap.sampleRateMasks&AUDIO_VDI_SAMPLE_RATE_MASK_48000))return 2;
    free(cap.formats);
    char usb_info[256]={0};
    result=a->GetExtraParams(a,AUDIO_VDI_EXT_PARAM_KEY_USB_DEVICE,"get_usb_info#C0D0",usb_info,sizeof(usb_info));
    printf("VDI_USB_INFO result=%d value=%s\n",result,usb_info);
    if(result || !strstr(usb_info,"sink_rate:48000;") || !strstr(usb_info,"sink_format:AUDIO_FORMAT_PCM_16_BIT;"))return 2;
    struct AudioRouteNodeVdi route_source={.role=AUDIO_VDI_PORT_SOURCE_ROLE,.type=AUDIO_VDI_PORT_MIX_TYPE};
    struct AudioRouteNodeVdi route_sink={.portId=0,.role=AUDIO_VDI_PORT_SINK_ROLE,.type=AUDIO_VDI_PORT_DEVICE_TYPE};
    route_sink.ext.device.type=PIN_VDI_OUT_SPEAKER;
    struct AudioRouteVdi route={.sources=&route_source,.sourcesLen=1,.sinks=&route_sink,.sinksLen=1};int32_t route_id=-1;
    int forbidden=a->UpdateAudioRoute(a,&route,&route_id);
    printf("VDI_SPEAKER_ROUTE_UNSUPPORTED result=%d handle=%d\n",forbidden,route_id);
    if(!forbidden || route_id!=-1)return 2;
    route_sink.ext.device.type=PIN_VDI_OUT_USB_EXT;result=a->UpdateAudioRoute(a,&route,&route_id);
    printf("VDI_USB_ROUTE result=%d handle=%d\n",result,route_id);if(result || route_id<0)return 2;
    struct AudioSampleAttributesVdi attrs={.type=AUDIO_VDI_IN_MEDIA,.interleaved=true,
        .format=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT,.sampleRate=48000,.channelCount=2,.frameSize=4,.isSignedData=true};
    if(pcm32) { attrs.format=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT;attrs.frameSize=8; }
    struct AudioDeviceDescriptorVdi device={.portId=0,.pins=PIN_VDI_OUT_USB_EXT,.desc="Actual USB PCM"};
    struct IAudioCaptureVdi *capture=NULL;
    int capture_result=a->CreateCapture(a,&device,&attrs,&capture);
    printf("VDI_CAPTURE_UNSUPPORTED result=%d null=%d\n",capture_result,capture==NULL);
    if(!capture_result || capture)return 2;
    struct IAudioRenderVdi *render=NULL;result=a->CreateRender(a,&device,&attrs,&render);
    printf("VDI_RENDER_CREATE result=%d\n",result);if(result || !render || render->Start(render))return 2;
    uint64_t frame_bytes=0;result=render->GetFrameSize(render,&frame_bytes);
    if(result || frame_bytes!=attrs.frameSize)return 2;
    int8_t invalid_frame[8]={0};uint64_t invalid_done=999;
    if(!render->RenderFrame(render,invalid_frame,attrs.frameSize-1,&invalid_done) || invalid_done)return 2;
    uint32_t total=0;int16_t samples[1024*2];int32_t samples32[1024*2];
    while(total<36000) {
        uint32_t n=36000-total;if(n>1024)n=1024;
        /* Bound each call to segment edges so gain/mute changes are exact. */
        unsigned edge=total<2400?2400:total<14400?14400:total<26400?26400:total<31200?31200:36000;
        if(n>edge-total)n=edge-total;
        float volume=total<14400?1.0f:.5f;bool mute=total>=26400;
        if(render->SetVolume(render,volume) || render->SetMute(render,mute))return 2;
        for(unsigned i=0;i<n;++i) {
            unsigned position=total+i;int tone=position>=2400 && position<31200;
            samples[i*2]=tone?((((position-2400)/24)&1)?8192:-8192):0;
            samples[i*2+1]=tone?((((position-2400)/48)&1)?4096:-4096):0;
            samples32[i*2]=(int32_t)samples[i*2]*65536;
            samples32[i*2+1]=(int32_t)samples[i*2+1]*65536;
        }
        uint32_t bytes=n*attrs.frameSize;uint64_t done=0;
        result=render->RenderFrame(render,pcm32?(int8_t *)samples32:(int8_t *)samples,bytes,&done);
        if(result || done!=bytes) { printf("VDI_WRITE_FAIL result=%d accepted=%llu requested=%u\n",result,(unsigned long long)done,bytes);return 2; }
        total+=n;
    }
    enum AudioDrainNotifyTypeVdi drain;result=render->DrainBuffer(render,&drain);
    printf("VDI_DRAIN result=%d frames=%u\n",result,total);if(result)return 2;
    int destroyed=a->DestroyRender(a,render),route_released=a->ReleaseAudioRoute(a,route_id);
    int unloaded=m->UnloadAdapter(m,a),released=m->ReleaseAudioManagerObject(m);
    release_descs(descs,count);
    printf("VDI_RESULT frames=%u destroy=%d unload=%d release=%d\n",total,destroyed,unloaded,released);
    if(destroyed || route_released || unloaded || released)return 2;
    puts("VDI_REAL_PCM_SUBMISSION_PASS independent_waveform_check_required=1");return 0;
}
