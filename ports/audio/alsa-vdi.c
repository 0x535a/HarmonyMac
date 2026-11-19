/* SPDX-License-Identifier: Apache-2.0
 * ALSA playback VDI: USB cards, the built-in J514 macaudio card (speakers and
 * headphone jack), and an optional QEMU-only speaker route fixture.
 * J514 speakers stay behind the kernel volume interlock: a guard thread holds
 * the "Speaker Volume Unlock" lease and caps every amplifier at -20 dB. */
#define _GNU_SOURCE
#include "iaudio_manager_vdi.h"
#include "j514_dsp.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERROR (-1)
#define UNSUPPORTED (-3)
struct Adapter { struct IAudioAdapterVdi api; int card; uint32_t rates; unsigned renders;
    pthread_mutex_t route_lock;int32_t route_id;uint32_t next_route;enum AudioPortPinVdi route_pin;
    bool j514; snd_ctl_t *ctl; };
struct Render {
    struct IAudioRenderVdi api; struct Adapter *adapter; snd_pcm_t *pcm;
    struct AudioSampleAttributesVdi attrs; uint64_t written;
    float volume,gain; bool mute,started,failed; pthread_mutex_t lock;
    /* J514: device 1 = six speakers, device 0 = headphone jack. */
    int device; unsigned hw_channels; snd_pcm_format_t hw_format; uint64_t jack_check;
    struct J514Dsp dsp;
};
static int usb_card(int card)
{
    char path[160],driver[512]={0};struct stat st;
    snprintf(path,sizeof(path),"/sys/class/sound/card%d/device/driver",card);
    ssize_t n=readlink(path,driver,sizeof(driver)-1);if(n<=0)return 0;driver[n]=0;
    if(!strstr(driver,"/snd-usb-audio"))return 0;
    snprintf(path,sizeof(path),"/dev/snd/pcmC%dD0p",card);
    return !stat(path,&st) && S_ISCHR(st.st_mode);
}
static bool has_cmdline_token(const char *command,const char *token)
{
    size_t length=strlen(token);const char *at=command;
    while((at=strstr(at,token))) {
        if((at==command || at[-1]==' ') && (!at[length] || at[length]==' ' || at[length]=='\n'))return true;
        at+=length;
    }
    return false;
}
static bool read_cmdline(char command[8192])
{
    FILE *file=fopen("/proc/cmdline","r");
    if(!file)return false;
    size_t bytes=fread(command,1,8191,file);
    fclose(file);
    return bytes && bytes<8191;
}
static bool fixture_card(int card)
{
#ifdef AURORA_J514_AUDIO_FIXTURE
    char command[8192]={0};
    if(!read_cmdline(command) || !has_cmdline_token(command,"aurora.virtual_identity=1") ||
       !has_cmdline_token(command,"aurora.j514_audio_fixture=1"))return false;
    char path[160],driver[512],id[32]={0};
    snprintf(path,sizeof(path),"/sys/class/sound/card%d/device/driver",card);
    ssize_t n=readlink(path,driver,sizeof(driver)-1);
    if(n<=0)return false;
    driver[n]=0;
    const char *name=strrchr(driver,'/');
    if(!name || strcmp(name+1,"snd_aloop"))return false;
    snprintf(path,sizeof(path),"/sys/class/sound/card%d/id",card);
    FILE *file=fopen(path,"r");
    if(!file)return false;
    bool matches=fgets(id,sizeof(id),file) && !strcmp(id,"AuroraJ514Lab\n");
    fclose(file);
    struct stat st;
    snprintf(path,sizeof(path),"/dev/snd/pcmC%dD0p",card);
    return matches && !stat(path,&st) && S_ISCHR(st.st_mode);
#else
    (void)card;
    return false;
#endif
}
static bool j514_card(int card)
{
#ifdef AURORA_J514_AUDIO_EXPERIMENTAL
    char path[160],id[32]={0};struct stat st;
    snprintf(path,sizeof(path),"/sys/class/sound/card%d/id",card);
    FILE *file=fopen(path,"r");if(!file)return false;
    bool matches=fgets(id,sizeof(id),file) && !strcmp(id,"AppleJ514\n");
    fclose(file);if(!matches)return false;
    snprintf(path,sizeof(path),"/dev/snd/pcmC%dD0p",card);
    if(stat(path,&st) || !S_ISCHR(st.st_mode))return false;
    snprintf(path,sizeof(path),"/dev/snd/pcmC%dD1p",card);
    return !stat(path,&st) && S_ISCHR(st.st_mode);
#else
    (void)card;
    return false;
#endif
}
static bool pcm_available(int card) { return usb_card(card) || fixture_card(card) || j514_card(card); }
static bool output_allowed(int card)
{
    char command[8192]={0};
    return pcm_available(card) && read_cmdline(command) &&
        !has_cmdline_token(command,"aurora.audio_disabled=1");
}
static bool route_matches(int card, enum AudioPortPinVdi pin)
{
    if(fixture_card(card))return pin==PIN_VDI_OUT_SPEAKER;
    if(j514_card(card))return pin==PIN_VDI_OUT_SPEAKER || pin==PIN_VDI_OUT_HEADSET || pin==PIN_VDI_OUT_HEADPHONE;
    return usb_card(card) && (pin==PIN_VDI_OUT_USB || pin==PIN_VDI_OUT_USB_EXT ||
        pin==PIN_VDI_OUT_USB_HEADSET);
}
static int open_pcm_device(int card,int device,snd_pcm_t **pcm)
{
    if(!output_allowed(card))return ERROR;
    char name[48];snprintf(name,sizeof(name),"hw:%d,%d",card,device);
    return snd_pcm_open(pcm,name,SND_PCM_STREAM_PLAYBACK,0);
}
static int open_pcm(int card,snd_pcm_t **pcm)
{
    if(!output_allowed(card)) {
        static unsigned denied=0;
        if(denied++<5)fprintf(stderr,"AURORA_AUDIO_OUTPUT_DISABLED explicit_boot_stop_or_unavailable_device=1 card=%d\n",card);
        return ERROR;
    }
    char name[48];snprintf(name,sizeof(name),"hw:%d,0",card);
    return snd_pcm_open(pcm,name,SND_PCM_STREAM_PLAYBACK,0);
}
static uint32_t probe_j514_rates(int card)
{
    snd_pcm_t *pcm=NULL;snd_pcm_hw_params_t *params;uint32_t rates=0;
    snd_pcm_hw_params_alloca(&params);
    if(open_pcm_device(card,1,&pcm)<0)return 0;
    if(snd_pcm_hw_params_any(pcm,params)>=0 && !snd_pcm_hw_params_test_channels(pcm,params,6) &&
       !snd_pcm_hw_params_test_access(pcm,params,SND_PCM_ACCESS_RW_INTERLEAVED)) {
        if(!snd_pcm_hw_params_test_rate(pcm,params,48000,0))rates|=AUDIO_VDI_SAMPLE_RATE_MASK_48000;
        if(!snd_pcm_hw_params_test_rate(pcm,params,44100,0))rates|=AUDIO_VDI_SAMPLE_RATE_MASK_44100;
    }
    snd_pcm_close(pcm);
    fprintf(stderr,"AURORA_J514_RATES card=%d rates=0x%x\n",card,rates);
    return rates;
}
static uint32_t probe_rates(int card)
{
    if(j514_card(card))return probe_j514_rates(card);
    snd_pcm_t *pcm=NULL;snd_pcm_hw_params_t *params=NULL;uint32_t rates=0;
    if(open_pcm(card,&pcm)<0)return 0;
    if(snd_pcm_hw_params_malloc(&params)<0)goto done;
    if(snd_pcm_hw_params_any(pcm,params)<0 ||
       snd_pcm_hw_params_test_format(pcm,params,SND_PCM_FORMAT_S16_LE)<0 ||
       snd_pcm_hw_params_test_channels(pcm,params,2)<0 ||
       snd_pcm_hw_params_test_access(pcm,params,SND_PCM_ACCESS_RW_INTERLEAVED)<0)goto done;
    if(!fixture_card(card) && !snd_pcm_hw_params_test_rate(pcm,params,44100,0))
        rates|=AUDIO_VDI_SAMPLE_RATE_MASK_44100;
    if(!snd_pcm_hw_params_test_rate(pcm,params,48000,0))rates|=AUDIO_VDI_SAMPLE_RATE_MASK_48000;
done:
    if(params)snd_pcm_hw_params_free(params);snd_pcm_close(pcm);return rates;
}
/* ---- J514 built-in card -------------------------------------------------- */
#define J514_SPEAKER_DEVICE 1
#define J514_JACK_DEVICE 0
#define J514_SPEAKER_CHANNELS 6
#define SPEAKER_MAGIC_VALUE ((long)(int32_t)0xdec1be15)
/* macaudio SN012776 scale: 201 = 0 dB, 2 steps per dB. Asahi safe_vol is -20 dB. */
#define J514_SAFE_SPEAKER_VOLUME 161
#define J514_LEASE_PING_US 60000 /* kernel lease is 250 ms */

static int ctl_find(snd_ctl_t *ctl,const char *name,snd_ctl_elem_id_t *id,snd_ctl_elem_info_t *info)
{
    snd_ctl_elem_id_clear(id);snd_ctl_elem_id_set_interface(id,SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id,name);snd_ctl_elem_info_set_id(info,id);
    if(snd_ctl_elem_info(ctl,info)<0) {
        snd_ctl_elem_id_set_interface(id,SND_CTL_ELEM_IFACE_CARD);snd_ctl_elem_info_set_id(info,id);
        if(snd_ctl_elem_info(ctl,info)<0)return ERROR;
    }
    snd_ctl_elem_info_get_id(info,id);return 0;
}
static int ctl_set_bool(snd_ctl_t *ctl,const char *name,bool on)
{
    snd_ctl_elem_id_t *id;snd_ctl_elem_info_t *info;snd_ctl_elem_value_t *value;
    snd_ctl_elem_id_alloca(&id);snd_ctl_elem_info_alloca(&info);snd_ctl_elem_value_alloca(&value);
    if(ctl_find(ctl,name,id,info))return ERROR;
    snd_ctl_elem_value_set_id(value,id);
    for(unsigned i=0;i<snd_ctl_elem_info_get_count(info);++i)snd_ctl_elem_value_set_boolean(value,i,on);
    return snd_ctl_elem_write(ctl,value)<0?ERROR:0;
}
static int ctl_set_enum(snd_ctl_t *ctl,const char *name,const char *item)
{
    snd_ctl_elem_id_t *id;snd_ctl_elem_info_t *info;snd_ctl_elem_value_t *value;
    snd_ctl_elem_id_alloca(&id);snd_ctl_elem_info_alloca(&info);snd_ctl_elem_value_alloca(&value);
    if(ctl_find(ctl,name,id,info) || snd_ctl_elem_info_get_type(info)!=SND_CTL_ELEM_TYPE_ENUMERATED)return ERROR;
    for(unsigned n=0;n<snd_ctl_elem_info_get_items(info);++n) {
        snd_ctl_elem_info_set_item(info,n);
        if(snd_ctl_elem_info(ctl,info)<0)return ERROR;
        if(strcmp(snd_ctl_elem_info_get_item_name(info),item))continue;
        snd_ctl_elem_value_set_id(value,id);
        for(unsigned i=0;i<snd_ctl_elem_info_get_count(info);++i)snd_ctl_elem_value_set_enumerated(value,i,n);
        return snd_ctl_elem_write(ctl,value)<0?ERROR:0;
    }
    return ERROR;
}
/* Asahi UCM: macaudio/laptops/HiFi.conf and cs42l84/Headphones.conf. */
static void j514_setup_mixer(snd_ctl_t *ctl)
{
    int r[6]={ctl_set_enum(ctl,"Headphone Playback Mux","Primary"),ctl_set_enum(ctl,"Speaker Playback Mux","Secondary"),
        ctl_set_bool(ctl,"Speaker Switch",true),ctl_set_bool(ctl,"Headphone Switch",true),
        ctl_set_enum(ctl,"Jack DACA Select","ASP RX CH1"),ctl_set_enum(ctl,"Jack DACB Select","ASP RX CH2")};
    fprintf(stderr,"AURORA_J514_MIXER hp_mux=%d spk_mux=%d spk_switch=%d hp_switch=%d daca=%d dacb=%d\n",r[0],r[1],r[2],r[3],r[4],r[5]);
}
static bool j514_headphones(snd_ctl_t *ctl)
{
    if(!ctl)return false;
    snd_ctl_elem_id_t *id;snd_ctl_elem_info_t *info;snd_ctl_elem_value_t *value;
    snd_ctl_elem_id_alloca(&id);snd_ctl_elem_info_alloca(&info);snd_ctl_elem_value_alloca(&value);
    if(ctl_find(ctl,"Headphone Jack",id,info))return false;
    snd_ctl_elem_value_set_id(value,id);
    return snd_ctl_elem_read(ctl,value)>=0 && snd_ctl_elem_value_get_boolean(value,0);
}

/* Speaker lease. The kernel limits every "* Speaker Volume" to 1 (silence)
 * unless one control file locks the unlock control and all speaker volumes and
 * pings it at least every 250 ms. If this process dies the kernel relocks. */
static void *j514_guard(void *arg)
{
    int card=(int)(intptr_t)arg;char name[32];snprintf(name,sizeof(name),"hw:%d",card);
    snd_ctl_t *ctl=NULL;
    if(snd_ctl_open(&ctl,name,0)<0) { fprintf(stderr,"AURORA_J514_GUARD open_failed card=%d\n",card);return NULL; }
    snd_ctl_elem_list_t *list;snd_ctl_elem_list_alloca(&list);
    if(snd_ctl_elem_list(ctl,list)<0 || snd_ctl_elem_list_alloc_space(list,snd_ctl_elem_list_get_count(list))<0 ||
       snd_ctl_elem_list(ctl,list)<0) { fprintf(stderr,"AURORA_J514_GUARD list_failed\n");snd_ctl_close(ctl);return NULL; }
    snd_ctl_elem_id_t *unlock;snd_ctl_elem_id_alloca(&unlock);bool have_unlock=false;
    snd_ctl_elem_id_t *volumes[16];unsigned count=0;
    for(unsigned i=0;i<snd_ctl_elem_list_get_used(list);++i) {
        const char *n=snd_ctl_elem_list_get_name(list,i);size_t len=strlen(n);
        if(!strcmp(n,"Speaker Volume Unlock")) { snd_ctl_elem_list_get_id(list,i,unlock);have_unlock=true; }
        else if(len>15 && !strcmp(n+len-15," Speaker Volume") && count<16) {
            if(snd_ctl_elem_id_malloc(&volumes[count])<0)break;
            snd_ctl_elem_list_get_id(list,i,volumes[count++]);
        }
    }
    snd_ctl_elem_list_free_space(list);
    if(!have_unlock || !count) {
        fprintf(stderr,"AURORA_J514_GUARD no_interlock unlock=%d volumes=%u (speakers left as the kernel set them)\n",have_unlock,count);
        for(unsigned i=0;i<count;++i)snd_ctl_elem_id_free(volumes[i]);snd_ctl_close(ctl);return NULL;
    }
    int locked=snd_ctl_elem_lock(ctl,unlock);
    for(unsigned i=0;i<count && locked>=0;++i)locked=snd_ctl_elem_lock(ctl,volumes[i]);
    fprintf(stderr,"AURORA_J514_GUARD lock=%d volumes=%u safe_volume=%d\n",locked,count,J514_SAFE_SPEAKER_VOLUME);
    if(locked<0) { for(unsigned i=0;i<count;++i)snd_ctl_elem_id_free(volumes[i]);snd_ctl_close(ctl);return NULL; }
    snd_ctl_elem_value_t *ping,*value;snd_ctl_elem_value_alloca(&ping);snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_info_t *info;snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_set_id(ping,unlock);snd_ctl_elem_value_set_integer(ping,0,SPEAKER_MAGIC_VALUE);
    bool capped=false;unsigned failures=0;
    for(;;) {
        int result=snd_ctl_elem_write(ctl,ping);
        if(result<0) {
            capped=false; /* relocked at some point: volumes were clamped */
            if(failures++<20)fprintf(stderr,"AURORA_J514_GUARD ping=%d\n",result);
        } else if(!capped) {
            int written=0;
            for(unsigned i=0;i<count;++i) {
                snd_ctl_elem_info_set_id(info,volumes[i]);
                if(snd_ctl_elem_info(ctl,info)<0)continue;
                long target=J514_SAFE_SPEAKER_VOLUME;
                if(target>snd_ctl_elem_info_get_max(info))target=snd_ctl_elem_info_get_max(info);
                snd_ctl_elem_value_set_id(value,volumes[i]);
                for(unsigned c=0;c<snd_ctl_elem_info_get_count(info);++c)snd_ctl_elem_value_set_integer(value,c,target);
                if(snd_ctl_elem_write(ctl,value)>=0)++written;
            }
            capped=written==(int)count;
            fprintf(stderr,"AURORA_J514_GUARD unlocked volumes_written=%d/%u\n",written,count);
        }
        usleep(J514_LEASE_PING_US);
    }
    return NULL;
}
static int guard_card=-1;
static void start_guard_once(void)
{
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(pthread_create(&thread,&attr,j514_guard,(void *)(intptr_t)guard_card))fprintf(stderr,"AURORA_J514_GUARD thread_failed\n");
    pthread_attr_destroy(&attr);
}
static void j514_start_guard(int card)
{
    static pthread_once_t once=PTHREAD_ONCE_INIT;
    if(guard_card<0)guard_card=card;
    pthread_once(&once,start_guard_once);
}

static int j514_configure(struct Render *r)
{
    snd_pcm_hw_params_t *hw;snd_pcm_hw_params_alloca(&hw);
    unsigned channels=r->device==J514_SPEAKER_DEVICE?J514_SPEAKER_CHANNELS:2;
    static const snd_pcm_format_t formats[]={SND_PCM_FORMAT_S32_LE,SND_PCM_FORMAT_S24_LE,SND_PCM_FORMAT_S16_LE};
    int result=snd_pcm_hw_params_any(r->pcm,hw);
    if(result>=0)result=snd_pcm_hw_params_set_access(r->pcm,hw,SND_PCM_ACCESS_RW_INTERLEAVED);
    if(result>=0)result=snd_pcm_hw_params_set_channels(r->pcm,hw,channels);
    if(result>=0)result=snd_pcm_hw_params_set_rate(r->pcm,hw,r->attrs.sampleRate,0);
    if(result<0)return result;
    result=-EINVAL;
    for(unsigned i=0;i<3 && result<0;++i)if(!snd_pcm_hw_params_test_format(r->pcm,hw,formats[i])) {
        result=snd_pcm_hw_params_set_format(r->pcm,hw,formats[i]);if(result>=0)r->hw_format=formats[i];
    }
    unsigned buffer_us=100000;
    if(result>=0)result=snd_pcm_hw_params_set_buffer_time_near(r->pcm,hw,&buffer_us,NULL);
    if(result>=0)result=snd_pcm_hw_params(r->pcm,hw);
    if(result<0)return result;
    r->hw_channels=channels;j514_dsp_init(&r->dsp,r->attrs.sampleRate);
    snd_pcm_sw_params_t *sw;snd_pcm_sw_params_alloca(&sw);
    if((result=snd_pcm_sw_params_current(r->pcm,sw))>=0 &&
       (result=snd_pcm_sw_params_set_tstamp_mode(r->pcm,sw,SND_PCM_TSTAMP_ENABLE))>=0 &&
       (result=snd_pcm_sw_params_set_tstamp_type(r->pcm,sw,SND_PCM_TSTAMP_TYPE_MONOTONIC))>=0)
        result=snd_pcm_sw_params(r->pcm,sw);
    fprintf(stderr,"AURORA_J514_PCM device=%d channels=%u format=%s rate=%u result=%d\n",r->device,channels,
        snd_pcm_format_name(r->hw_format),r->attrs.sampleRate,result);
    return result;
}
/* Follow the headphone jack like a laptop: plugged headphones take the output. */
static int j514_select(struct Render *r,bool reopen)
{
    int device=j514_headphones(r->adapter->ctl)?J514_JACK_DEVICE:J514_SPEAKER_DEVICE;
    if(!reopen && r->pcm && device==r->device)return 0;
    if(r->pcm) { snd_pcm_drop(r->pcm);snd_pcm_close(r->pcm);r->pcm=NULL; }
    r->device=device;
    if(open_pcm_device(r->adapter->card,device,&r->pcm)<0 || j514_configure(r)<0) {
        if(r->pcm) { snd_pcm_close(r->pcm);r->pcm=NULL; }
        return ERROR;
    }
    fprintf(stderr,"AURORA_J514_ROUTE device=%s\n",device==J514_JACK_DEVICE?"headphones":"speakers");
    return 0;
}
static void put_sample(uint8_t *out,snd_pcm_format_t format,double value)
{
    if(value>1)value=1;if(value< -1)value=-1;
    if(format==SND_PCM_FORMAT_S16_LE) { int16_t v=(int16_t)lrint(value*32767);memcpy(out,&v,2); }
    else if(format==SND_PCM_FORMAT_S24_LE) { int32_t v=(int32_t)lrint(value*8388607);memcpy(out,&v,4); }
    else { int32_t v=(int32_t)lrint(value*2147483647.0);memcpy(out,&v,4); }
}
static int j514_write(struct Render *r,const uint8_t *source,uint32_t frames,uint64_t *done)
{
    /* Re-check the jack about every 250 ms of audio. */
    if((r->jack_check+=frames)>=r->attrs.sampleRate/4) {
        r->jack_check=0;
        int before=r->device;
        if(j514_select(r,false)<0)return ERROR;
        if(r->device!=before && snd_pcm_prepare(r->pcm)<0)return ERROR;
    }
    const unsigned bytes=r->hw_format==SND_PCM_FORMAT_S16_LE?2:4,stride=bytes*r->hw_channels;
    uint8_t out[512*J514_SPEAKER_CHANNELS*4];
    while(frames) {
        uint32_t count=frames>512?512:frames;double scale=r->mute?0:r->volume*r->gain;
        for(uint32_t i=0;i<count;++i) {
            double in[2];
            for(int ch=0;ch<2;++ch) {
                if(r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT) { int32_t v;memcpy(&v,source+(i*2+ch)*4,4);in[ch]=v/2147483648.0; }
                else { int16_t v;memcpy(&v,source+(i*2+ch)*2,2);in[ch]=v/32768.0; }
                in[ch]*=scale;
            }
            uint8_t *frame=out+i*stride;
            if(r->hw_channels==2) { put_sample(frame,r->hw_format,in[0]);put_sample(frame+bytes,r->hw_format,in[1]);continue; }
            double map[6];j514_dsp_frame(&r->dsp,in,map);
            for(int ch=0;ch<6;++ch)put_sample(frame+ch*bytes,r->hw_format,map[ch]);
        }
        snd_pcm_sframes_t written=snd_pcm_writei(r->pcm,out,count);
        if(written==-EPIPE && snd_pcm_prepare(r->pcm)>=0)continue;
        if(written<=0 || (snd_pcm_uframes_t)written>count)return ERROR;
        r->written+=(uint64_t)written;*done+=(uint64_t)written*r->attrs.frameSize;
        frames-=(uint32_t)written;source+=written*r->attrs.frameSize;
    }
    return 0;
}

static int configure_pcm(struct Render *r)
{
    int result=snd_pcm_set_params(r->pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,
        r->attrs.channelCount,r->attrs.sampleRate,0,100000);
    if(result<0)return result;
    snd_pcm_sw_params_t *sw=NULL;
    if((result=snd_pcm_sw_params_malloc(&sw))<0)return result;
    if((result=snd_pcm_sw_params_current(r->pcm,sw))>=0 &&
       (result=snd_pcm_sw_params_set_tstamp_mode(r->pcm,sw,SND_PCM_TSTAMP_ENABLE))>=0 &&
       (result=snd_pcm_sw_params_set_tstamp_type(r->pcm,sw,SND_PCM_TSTAMP_TYPE_MONOTONIC))>=0)
        result=snd_pcm_sw_params(r->pcm,sw);
    snd_pcm_sw_params_free(sw);return result;
}
static int32_t render_frame(struct IAudioRenderVdi *self,const int8_t *data,uint32_t bytes,uint64_t *done)
{
    struct Render *r=(struct Render *)self;if(done)*done=0;
    if(!r || !data || !done || !r->attrs.frameSize || bytes%r->attrs.frameSize)return ERROR;
    pthread_mutex_lock(&r->lock);
    if(!r->started || r->failed || !output_allowed(r->adapter->card)) { pthread_mutex_unlock(&r->lock);return ERROR; }
    const uint8_t *source=(const uint8_t *)data;uint32_t remaining=bytes/r->attrs.frameSize;
    if(r->adapter->j514) {
        int result=j514_write(r,source,remaining,done);
        if(result)r->failed=true;
        pthread_mutex_unlock(&r->lock);return result;
    }
    while(remaining) {
        int16_t adjusted[2048];uint32_t count=remaining>1024?1024:remaining;
        float scale=r->mute?0:r->volume*r->gain;
        for(uint32_t i=0;i<count*2;++i) {
            double value;
            if(r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT) {
                int32_t sample;memcpy(&sample,source+i*4,4);value=(double)sample*scale/65536.0;
            } else { int16_t sample;memcpy(&sample,source+i*2,2);value=(double)sample*scale; }
            if(value>32767)value=32767;if(value< -32768)value= -32768;
            adjusted[i]=(int16_t)value;
        }
        snd_pcm_sframes_t written=snd_pcm_writei(r->pcm,adjusted,count);
        if(written<=0 || (snd_pcm_uframes_t)written>count) {
            r->failed=true;pthread_mutex_unlock(&r->lock);return ERROR;
        }
        r->written+=(uint64_t)written;*done+=(uint64_t)written*r->attrs.frameSize;
        remaining-=(uint32_t)written;source+=written*r->attrs.frameSize;
    }
    pthread_mutex_unlock(&r->lock);return 0;
}
static int32_t start(struct IAudioRenderVdi *self)
{
    struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);
    if(r->started && !r->failed) { pthread_mutex_unlock(&r->lock);return 0; }
    int result=output_allowed(r->adapter->card)?0:ERROR;
    if(!result && r->adapter->j514)result=j514_select(r,r->failed || !r->pcm);
    if(!result)result=snd_pcm_prepare(r->pcm);
    fprintf(stderr,"AURORA_AUDIO_START pid=%d uid=%u card=%d prepare=%d\n",getpid(),getuid(),r->adapter->card,result);
    if(result>=0) { r->written=0;r->started=true;r->failed=false; }
    pthread_mutex_unlock(&r->lock);return result<0?ERROR:0;
}
static int32_t flush_render(struct IAudioRenderVdi *self)
{
    struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);
    int result=snd_pcm_drop(r->pcm);if(result>=0)result=snd_pcm_prepare(r->pcm);
    if(result>=0) { r->written=0;r->failed=false; }
    pthread_mutex_unlock(&r->lock);return result<0?ERROR:0;
}
static int32_t stop(struct IAudioRenderVdi *self)
{
    struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);
    int result=snd_pcm_drop(r->pcm);r->started=false;
    pthread_mutex_unlock(&r->lock);return result<0?ERROR:0;
}
static int32_t drain(struct IAudioRenderVdi *self,enum AudioDrainNotifyTypeVdi *type)
{
    struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);
    int result=r->failed?ERROR:snd_pcm_drain(r->pcm);r->started=false;
    if(result>=0 && type)*type=AUDIO_VDI_DRAIN_NORMAL_MODE;
    pthread_mutex_unlock(&r->lock);return result<0?ERROR:0;
}
static int32_t latency(struct IAudioRenderVdi *self,uint32_t *ms)
{
    struct Render *r=(struct Render *)self;if(!ms)return ERROR;snd_pcm_sframes_t delay=0;
    pthread_mutex_lock(&r->lock);int result=snd_pcm_delay(r->pcm,&delay);
    if(result>=0 && delay>=0)*ms=(uint32_t)((uint64_t)delay*1000/r->attrs.sampleRate);
    pthread_mutex_unlock(&r->lock);return result<0 || delay<0?ERROR:0;
}
static int32_t position(struct IAudioRenderVdi *self,uint64_t *frames,struct AudioTimeStampVdi *stamp)
{
    struct Render *r=(struct Render *)self;if(!frames || !stamp)return ERROR;
    snd_pcm_uframes_t avail=0;snd_htimestamp_t time={0};snd_pcm_sframes_t delay=0;
    pthread_mutex_lock(&r->lock);
    int result=r->failed?ERROR:snd_pcm_htimestamp(r->pcm,&avail,&time);
    if(result>=0)result=snd_pcm_delay(r->pcm,&delay);
    if(result>=0 && delay>=0 && (uint64_t)delay<=r->written && time.tv_sec>0) {
        *frames=r->written-(uint64_t)delay;stamp->tvSec=time.tv_sec;stamp->tvNSec=time.tv_nsec;
    } else result=ERROR;
    pthread_mutex_unlock(&r->lock);return result<0?ERROR:0;
}
static int32_t volume_set(struct IAudioRenderVdi *self,float value)
{ if(!(value>=0 && value<=1))return ERROR;struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);r->volume=value;pthread_mutex_unlock(&r->lock);return 0; }
static int32_t volume_get(struct IAudioRenderVdi *self,float *value)
{ if(!value)return ERROR;struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);*value=r->volume;pthread_mutex_unlock(&r->lock);return 0; }
static int32_t mute_set(struct IAudioRenderVdi *self,bool value)
{ struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);r->mute=value;pthread_mutex_unlock(&r->lock);return 0; }
static int32_t mute_get(struct IAudioRenderVdi *self,bool *value)
{ if(!value)return ERROR;struct Render *r=(struct Render *)self;pthread_mutex_lock(&r->lock);*value=r->mute;pthread_mutex_unlock(&r->lock);return 0; }
static int32_t frame_size(struct IAudioRenderVdi *self,uint64_t *value)
{ if(!value)return ERROR;*value=((struct Render *)self)->attrs.frameSize;return 0; }
static int32_t frame_count(struct IAudioRenderVdi *self,uint64_t *value)
{ if(!value)return ERROR;snd_pcm_uframes_t buffer,period;int result=snd_pcm_get_params(((struct Render *)self)->pcm,&buffer,&period);if(result<0)return ERROR;*value=buffer;return 0; }
static int32_t buffer_size(struct IAudioRenderVdi *self,uint64_t *value)
{ int result=frame_count(self,value);if(!result)*value*=((struct Render *)self)->attrs.frameSize;return result; }
static int32_t attrs_get(struct IAudioRenderVdi *self,struct AudioSampleAttributesVdi *attrs)
{ if(!attrs)return ERROR;*attrs=((struct Render *)self)->attrs;return 0; }
static int32_t supports_drain(struct IAudioRenderVdi *self,bool *value)
{ (void)self;if(!value)return ERROR;*value=true;return 0; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NO(name,args) static int32_t name args { return UNSUPPORTED; }
NO(speed_set,(struct IAudioRenderVdi *s,float v))
NO(speed_get,(struct IAudioRenderVdi *s,float *v))
NO(mode_set,(struct IAudioRenderVdi *s,enum AudioChannelModeVdi v))
NO(mode_get,(struct IAudioRenderVdi *s,enum AudioChannelModeVdi *v))
NO(callback,(struct IAudioRenderVdi *s,RenderCallbackVdi f,void *v))
NO(scene_check,(struct IAudioRenderVdi *s,const struct AudioSceneDescriptorVdi *v,bool *b))
NO(scene_set,(struct IAudioRenderVdi *s,const struct AudioSceneDescriptorVdi *v))
NO(gain_limits,(struct IAudioRenderVdi *s,float *a,float *b))
NO(gain_get,(struct IAudioRenderVdi *s,float *a))
NO(gain_set,(struct IAudioRenderVdi *s,float a))
NO(attrs_set,(struct IAudioRenderVdi *s,const struct AudioSampleAttributesVdi *a))
NO(channel_get,(struct IAudioRenderVdi *s,uint32_t *a))
NO(extra_set,(struct IAudioRenderVdi *s,const char *v))
NO(extra_get,(struct IAudioRenderVdi *s,char *v,uint32_t n))
NO(mmap_buffer,(struct IAudioRenderVdi *s,int32_t n,struct AudioMmapBufferDescriptorVdi *v))
NO(mmap_position,(struct IAudioRenderVdi *s,uint64_t *n,struct AudioTimeStampVdi *t))
NO(effect,(struct IAudioRenderVdi *s,uint64_t id))
NO(pause_render,(struct IAudioRenderVdi *s))
NO(dump_render,(struct IAudioRenderVdi *s,int32_t range,int32_t fd))
NO(set_buffer,(struct IAudioRenderVdi *s,uint32_t n))
static int32_t pause_supported(struct IAudioRenderVdi *s,bool *p,bool *r)
{ if(!p || !r)return ERROR;*p=false;*r=false;return 0; }
static int32_t capture_create(struct IAudioAdapterVdi *s,const struct AudioDeviceDescriptorVdi *d,const struct AudioSampleAttributesVdi *a,struct IAudioCaptureVdi **o)
{ if(o)*o=NULL;return UNSUPPORTED; }
NO(capture_destroy,(struct IAudioAdapterVdi *s,struct IAudioCaptureVdi *c))
NO(pass_set,(struct IAudioAdapterVdi *s,const struct AudioPortVdi *p,enum AudioPortPassthroughModeVdi m))
NO(pass_get,(struct IAudioAdapterVdi *s,const struct AudioPortVdi *p,enum AudioPortPassthroughModeVdi *m))
NO(device_status,(struct IAudioAdapterVdi *s,struct AudioDeviceStatusVdi *v))
static int32_t route_update(struct IAudioAdapterVdi *s,const struct AudioRouteVdi *r,int32_t *h)
{
    struct Adapter *a=(struct Adapter *)s;
    if(!a || !r || !h || !r->sources || !r->sinks || r->sourcesLen!=1 || r->sinksLen!=1)return ERROR;
    const struct AudioRouteNodeVdi *source=r->sources,*sink=r->sinks;
    if(source->type!=AUDIO_VDI_PORT_MIX_TYPE || source->role!=AUDIO_VDI_PORT_SOURCE_ROLE ||
        sink->type!=AUDIO_VDI_PORT_DEVICE_TYPE || sink->role!=AUDIO_VDI_PORT_SINK_ROLE || sink->portId!=0)return UNSUPPORTED;
    enum AudioPortPinVdi pin=sink->ext.device.type;
    if(!pcm_available(a->card))return ERROR;
    if(!route_matches(a->card,pin))return UNSUPPORTED;
    // Persist the framework route to this adapter's fixed PCM.
    pthread_mutex_lock(&a->route_lock);
    if(a->route_id<0) { a->next_route=(a->next_route%0x7ffffffeU)+1;a->route_id=(int32_t)a->next_route; }
    a->route_pin=pin;*h=a->route_id;
    fprintf(stderr,"AURORA_AUDIO_ROUTE card=%d pin=0x%x handle=%d\n",a->card,(unsigned)pin,*h);
    pthread_mutex_unlock(&a->route_lock);return 0;
}
static int32_t route_release(struct IAudioAdapterVdi *s,int32_t h)
{
    struct Adapter *a=(struct Adapter *)s;if(!a)return ERROR;pthread_mutex_lock(&a->route_lock);
    int result=h>=0 && h==a->route_id?0:ERROR;
    if(!result) { a->route_id=-1;a->route_pin=PIN_VDI_NONE; }
    pthread_mutex_unlock(&a->route_lock);return result;
}
NO(mic_set,(struct IAudioAdapterVdi *s,bool v))
NO(mic_get,(struct IAudioAdapterVdi *s,bool *v))
NO(voice_set,(struct IAudioAdapterVdi *s,float v))
NO(adapter_extra_set,(struct IAudioAdapterVdi *s,enum AudioExtParamKeyVdi k,const char *c,const char *v))
static int32_t adapter_extra_get(struct IAudioAdapterVdi *s,enum AudioExtParamKeyVdi k,const char *c,char *v,uint32_t n)
{
    struct Adapter *a=(struct Adapter *)s;if(v && n)v[0]=0;
    fprintf(stderr,"AURORA_AUDIO_EXTRA_GET key=%d condition=%s capacity=%u\n",(int)k,c?c:"(null)",n);
    if(!a || !c || !v || !n || k!=AUDIO_VDI_EXT_PARAM_KEY_USB_DEVICE)return UNSUPPORTED;
    int card=-1,device=-1,used=0;bool classify=false;
    if(sscanf(c,"need_change_usb_device#C%dD%d%n",&card,&device,&used)==2 && used>0 && !c[used])classify=true;
    else {
        used=0;
        if(sscanf(c,"get_usb_info#C%dD%d%n",&card,&device,&used)!=2 || used<=0 || c[used])return UNSUPPORTED;
    }
    if(card<0 || card>=32 || device!=0 || !usb_card(card))return ERROR;
    uint32_t rates=card==a->card?a->rates:probe_rates(card);if(!rates)return ERROR;
    int length;
    if(classify) {
        /* A real snd-usb-audio PCM stays on the framework's USB/ALSA route
         * rather than being handed to the old primary/hifi codec path. */
        length=snprintf(v,n,"false");
    } else {
        unsigned rate=(rates&AUDIO_VDI_SAMPLE_RATE_MASK_48000)?48000:44100;
        length=snprintf(v,n,"sink_rate:%u;sink_format:AUDIO_FORMAT_PCM_16_BIT;sink_channels:2;",rate);
    }
    if(length<0 || (uint32_t)length>=n) { v[0]=0;return ERROR; }
    return 0;
}
NO(observer,(struct IAudioAdapterVdi *s,struct IAudioCallbackVdi *c,int8_t cookie))
#pragma GCC diagnostic pop

static int32_t render_create(struct IAudioAdapterVdi *self,const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs,struct IAudioRenderVdi **out)
{
    if(out)*out=NULL;if(!desc || !attrs || !out || desc->portId!=0)return ERROR;
    if(!output_allowed(((struct Adapter *)self)->card))return UNSUPPORTED;
    fprintf(stderr,"AURORA_AUDIO_CREATE pid=%d uid=%u card=%d port=%u pins=0x%x format=%u rate=%u channels=%u interleaved=%d signed=%d big_endian=%d\n",
        getpid(),getuid(),((struct Adapter *)self)->card,desc->portId,(unsigned)desc->pins,
        (unsigned)attrs->format,attrs->sampleRate,attrs->channelCount,attrs->interleaved,attrs->isSignedData,attrs->isBigEndian);
    if(!route_matches(((struct Adapter *)self)->card,desc->pins))return UNSUPPORTED;
    if((attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT && attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT) ||
        attrs->channelCount!=2 || !attrs->interleaved || attrs->isBigEndian || !attrs->isSignedData)return UNSUPPORTED;
    struct Adapter *a=(struct Adapter *)self;uint32_t bit=attrs->sampleRate==48000?AUDIO_VDI_SAMPLE_RATE_MASK_48000:
        attrs->sampleRate==44100?AUDIO_VDI_SAMPLE_RATE_MASK_44100:0;
    if(!bit || !(bit&a->rates))return UNSUPPORTED;
    struct Render *r=calloc(1,sizeof(*r));if(!r)return ERROR;r->adapter=a;r->attrs=*attrs;
    r->attrs.frameSize=attrs->format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT?8:4;r->volume=1;r->gain=1;
    if(a->j514) {
        if(j514_select(r,true)<0) { free(r);return ERROR; }
    } else if(open_pcm(a->card,&r->pcm)<0 || configure_pcm(r)<0) { if(r->pcm)snd_pcm_close(r->pcm);free(r);return ERROR; }
    fprintf(stderr,"AURORA_AUDIO_PCM_OPEN pid=%d card=%d\n",getpid(),a->card);
    if(pthread_mutex_init(&r->lock,NULL)) { snd_pcm_close(r->pcm);free(r);return ERROR; }
    r->api=(struct IAudioRenderVdi){latency,render_frame,position,speed_set,speed_get,mode_set,mode_get,callback,
        drain,supports_drain,scene_check,scene_set,mute_set,mute_get,volume_set,volume_get,gain_limits,gain_get,gain_set,
        frame_size,frame_count,attrs_set,attrs_get,channel_get,extra_set,extra_get,mmap_buffer,mmap_position,effect,effect,
        buffer_size,start,stop,pause_render,pause_render,flush_render,stop,dump_render,pause_supported,set_buffer};
    ++a->renders;*out=&r->api;return 0;
}
static int32_t render_destroy(struct IAudioAdapterVdi *self,struct IAudioRenderVdi *render)
{
    if(!render)return ERROR;struct Render *r=(struct Render *)render;
    if(r->adapter!=(struct Adapter *)self)return ERROR;
    int result=r->pcm?snd_pcm_close(r->pcm):0;pthread_mutex_destroy(&r->lock);--r->adapter->renders;free(r);return result<0?ERROR:0;
}
static int32_t ports_init(struct IAudioAdapterVdi *self)
{ return pcm_available(((struct Adapter *)self)->card)?0:ERROR; }
static int32_t port_cap(struct IAudioAdapterVdi *self,const struct AudioPortVdi *port,struct AudioPortCapabilityVdi *cap)
{
    struct Adapter *a=(struct Adapter *)self;if(!port || !cap || port->portId!=0 || !pcm_available(a->card))return ERROR;
    memset(cap,0,sizeof(*cap));cap->formats=malloc(2*sizeof(*cap->formats));if(!cap->formats)return ERROR;
    cap->formats[0]=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;cap->formats[1]=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT;
    cap->formatsLen=cap->formatNum=2;
    cap->deviceType=fixture_card(a->card) || a->j514?AUDIO_VDI_PRIMARY_DEVICE:AUDIO_VDI_USB_DEVICE;
    cap->deviceId=0;cap->hardwareMode=false;cap->sampleRateMasks=a->rates;cap->channelMasks=AUDIO_VDI_CHANNEL_STEREO;cap->channelCount=2;return 0;
}
static int fixture_primary(void)
{
    for(int card=0;card<32;++card)if(fixture_card(card))return card;
    return -1;
}
static int j514_primary(void)
{
    for(int card=0;card<32;++card)if(j514_card(card))return card;
    return -1;
}
static int append_adapter(struct AudioAdapterDescriptorVdi *out,const char *name,const char *port)
{
    struct AudioAdapterDescriptorVdi d={0};
    d.adapterName=strdup(name);
    d.ports=calloc(1,sizeof(*d.ports));
    if(d.adapterName && d.ports) {
        d.ports[0]=(struct AudioPortVdi){PORT_VDI_OUT,0,strdup(port)};
        if(d.ports[0].portName) {
            d.portsLen=1;
            *out=d;
            return 0;
        }
    }
    free(d.adapterName);
    free(d.ports);
    return ERROR;
}
static int32_t manager_get(struct IAudioManagerVdi *self,struct AudioAdapterDescriptorVdi *descs,uint32_t *count)
{
    (void)self;if(!descs || !count || *count>AUDIO_VDI_ADAPTER_NUM_MAX)return ERROR;
    uint32_t capacity=*count,found=0;bool first=true;*count=0;
    const bool lab=fixture_primary()>=0,builtin=!lab && j514_primary()>=0,fixture=lab || builtin;
    if(fixture && capacity) {
        if(append_adapter(&descs[found],"primary",lab?"J514 DSP loopback fixture":"J514 built-in speakers and headphone jack"))return ERROR;
        *count=++found;
    }
    for(int card=0;card<32 && found<capacity;++card) {
        if(!usb_card(card))continue;
        /* The stock framework queries primary to classify USB cards.
         * Supply that view from the first USB card if no fixture owns primary. */
        unsigned views=first && !fixture?2:1;
        for(unsigned view=0;view<views && found<capacity;++view) {
            char name[64],port[64];
            if(first)strcpy(name,view==0 && !fixture?"primary":"usb");
            else snprintf(name,sizeof(name),"usb_%d",card);
            snprintf(port,sizeof(port),"Actual USB ALSA card %d playback",card);
            if(append_adapter(&descs[found],name,port))return ERROR;
            *count=++found;
        }
        first=false;
    }
    struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
    fprintf(stderr,"AURORA_AUDIO_ENUM pid=%d uid=%u mono=%lld.%09ld count=%u\n",getpid(),getuid(),(long long)now.tv_sec,now.tv_nsec,*count);
    return 0;
}
static int32_t manager_load(struct IAudioManagerVdi *self,const struct AudioAdapterDescriptorVdi *desc,struct IAudioAdapterVdi **out)
{
    (void)self;if(out)*out=NULL;if(!desc || !desc->adapterName || !out)return ERROR;
    int card=-1;
    if(!strcmp(desc->adapterName,"usb") || !strcmp(desc->adapterName,"primary")) {
        if(!strcmp(desc->adapterName,"primary")) { card=fixture_primary();if(card<0)card=j514_primary(); }
        if(card<0)for(int i=0;i<32;++i)if(usb_card(i)) { card=i;break; }
    }
    else { int n=0;if(sscanf(desc->adapterName,"usb_%d%n",&card,&n)!=1 || desc->adapterName[n] || card<0 || card>=32)return ERROR; }
    if(card<0 || !pcm_available(card))return ERROR;
    struct Adapter *a=calloc(1,sizeof(*a));if(!a)return ERROR;a->card=card;a->rates=probe_rates(card);
    if(!a->rates) { free(a);return ERROR; }
    if(pthread_mutex_init(&a->route_lock,NULL)) { free(a);return ERROR; }a->route_id=-1;
    a->j514=j514_card(card);
    if(a->j514) {
        char ctl_name[32];snprintf(ctl_name,sizeof(ctl_name),"hw:%d",card);
        if(snd_ctl_open(&a->ctl,ctl_name,0)<0)a->ctl=NULL;
        if(a->ctl)j514_setup_mixer(a->ctl);
        j514_start_guard(card);
    }
    fprintf(stderr,"AURORA_AUDIO_LOAD pid=%d uid=%u name=%s card=%d rates=0x%x\n",getpid(),getuid(),desc->adapterName,card,a->rates);
    a->api=(struct IAudioAdapterVdi){ports_init,render_create,render_destroy,capture_create,capture_destroy,port_cap,
        pass_set,pass_get,device_status,route_update,route_release,mic_set,mic_get,voice_set,adapter_extra_set,adapter_extra_get,observer};
    *out=&a->api;return 0;
}
static int32_t manager_unload(struct IAudioManagerVdi *self,struct IAudioAdapterVdi *adapter)
{ (void)self;if(!adapter || ((struct Adapter *)adapter)->renders)return ERROR;
  struct Adapter *a=(struct Adapter *)adapter;if(a->ctl)snd_ctl_close(a->ctl);
  pthread_mutex_destroy(&a->route_lock);free(adapter);return 0; }
static int32_t manager_release(struct IAudioManagerVdi *self) { (void)self;return 0; }
static struct IAudioManagerVdi manager={manager_get,manager_load,manager_unload,manager_release};
__attribute__((visibility("default"))) struct IAudioManagerVdi *AudioManagerCreateIfInstance(void) { return &manager; }
