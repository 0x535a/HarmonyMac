/* Host unit test for j514_dsp.h: channel map and crossover attenuation. */
#include "../audio/j514_dsp.h"
#include <stdio.h>
static double power(const double *x,int n,double f,double rate)
{
    double w=2*M_PI*f/rate,c=2*cos(w),s1=0,s2=0;
    for(int i=0;i<n;++i){double s=x[i]+c*s1-s2;s2=s1;s1=s;}
    return sqrt(s1*s1+s2*s2-c*s1*s2)/n*2;
}
int main(void)
{
    const double rates[2]={48000,44100};int failures=0;
    for(int r=0;r<2;++r){
        double rate=rates[r];struct J514Dsp d;j514_dsp_init(&d,rate);
        enum{N=48000};static double out[6][N];
        for(int i=0;i<N;++i){double in[2]={0.5*sin(2*M_PI*300*i/rate),0.5*sin(2*M_PI*5000*i/rate)},o[6];j514_dsp_frame(&d,in,o);for(int c=0;c<6;++c)out[c][i]=o[c];}
        const char *name[6]={"woofer_l1","woofer_r1","tweeter_l","tweeter_r","woofer_l2","woofer_r2"};
        double p300[6],p5k[6];
        for(int c=0;c<6;++c){p300[c]=power(out[c]+N/2,N/2,300,rate);p5k[c]=power(out[c]+N/2,N/2,5000,rate);
            printf("rate=%.0f %-10s 300Hz=%.4f (%.1f dB) 5kHz=%.4f (%.1f dB)\n",rate,name[c],p300[c],20*log10(p300[c]/0.5+1e-12),p5k[c],20*log10(p5k[c]/0.5+1e-12));}
        int ok=p300[0]>0.4&&p300[4]>0.4&&p300[0]==p300[4]&&p5k[1]<0.02&&p5k[5]<0.02&&p300[1]<1e-3&&p5k[0]<1e-3
             &&p5k[3]>0.45&&p300[2]<1e-3&&p5k[2]<1e-3&&p300[3]<1e-3;
        printf("rate=%.0f %s\n",rate,ok?"PASS":"FAIL");failures+=!ok;
    }
    return failures;
}
