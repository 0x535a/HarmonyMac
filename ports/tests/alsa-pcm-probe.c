/* Real OHOS-libasound -> actual USB PCM. No null/default device or pacing fallback. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout,NULL);alarm(30);
    int card=-1,selected=-1;
    while(snd_card_next(&card)>=0 && card>=0) {
        char path[160],driver[512]={0};
        snprintf(path,sizeof(path),"/sys/class/sound/card%d/device/driver",card);
        ssize_t n=readlink(path,driver,sizeof(driver)-1);
        if(n>0)driver[n]=0;
        printf("ALSA_CARD card=%d driver=%s\n",card,driver);
        if(strstr(driver,"/snd-usb-audio")) { selected=card;break; }
    }
    if(selected<0) { puts("ALSA_NO_USB_CARD");return 2; }
    char name[48];snprintf(name,sizeof(name),"hw:%d,0",selected);
    snd_pcm_t *pcm=NULL;int result=snd_pcm_open(&pcm,name,SND_PCM_STREAM_PLAYBACK,0);
    printf("ALSA_OPEN device=%s result=%d\n",name,result);
    if(result<0)return 2;
    result=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,48000,0,100000);
    printf("ALSA_CONFIG result=%d rate=48000 channels=2 format=S16_LE\n",result);
    if(result<0) { snd_pcm_close(pcm);return 2; }
    int16_t samples[1024*2];unsigned total=0;
    while(total<24000) {
        unsigned frames=24000-total;if(frames>1024)frames=1024;
        for(unsigned i=0;i<frames;++i) {
            unsigned position=total+i;
            samples[i*2]=((position/24)&1)?8192:-8192;
            samples[i*2+1]=((position/48)&1)?4096:-4096;
        }
        snd_pcm_sframes_t n=snd_pcm_writei(pcm,samples,frames);
        if(n<0) { printf("ALSA_WRITE_ERROR result=%ld\n",(long)n);snd_pcm_close(pcm);return 2; }
        if(n==0 || (unsigned long)n>frames) { snd_pcm_close(pcm);return 2; }
        total+=(unsigned)n;
    }
    result=snd_pcm_drain(pcm);int closed=snd_pcm_close(pcm);
    printf("ALSA_RESULT accepted_frames=%u drain=%d close=%d uid=%u\n",total,result,closed,getuid());
    if(result<0 || closed<0 || total!=24000)return 2;
    puts("ALSA_PCM_SUBMISSION_PASS wav_content_requires_separate_verification=1");return 0;
}
