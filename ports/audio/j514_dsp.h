/* SPDX-License-Identifier: Apache-2.0
 * J514 stereo -> six speaker channels: Linkwitz-Riley 4th order crossover at
 * 2 kHz, woofers additionally high-passed at 100 Hz. Output order matches the
 * macaudio TDM slots: left woofer 1, right woofer 1, left tweeter, right
 * tweeter, left woofer 2, right woofer 2. */
#pragma once
#include <math.h>

struct Biquad { double b0,b1,b2,a1,a2,z1,z2; };
struct J514Dsp { struct Biquad woofer[2][3], tweeter[2][2]; };

static inline struct Biquad j514_biquad(int highpass,double frequency,double rate,double q)
{
    /* RBJ audio EQ cookbook. */
    double w=2*M_PI*frequency/rate,c=cos(w),alpha=sin(w)/(2*q),a0=1+alpha;struct Biquad f={0};
    if(!highpass) { f.b0=(1-c)/2/a0;f.b1=(1-c)/a0;f.b2=f.b0; }
    else { f.b0=(1+c)/2/a0;f.b1=-(1+c)/a0;f.b2=f.b0; }
    f.a1=-2*c/a0;f.a2=(1-alpha)/a0;return f;
}
static inline double j514_run(struct Biquad *f,double x)
{ double y=f->b0*x+f->z1;f->z1=f->b1*x-f->a1*y+f->z2;f->z2=f->b2*x-f->a2*y;return y; }
static inline void j514_dsp_init(struct J514Dsp *d,double rate)
{
    const double q=M_SQRT1_2;
    for(int ch=0;ch<2;++ch) {
        d->woofer[ch][0]=j514_biquad(0,2000,rate,q);d->woofer[ch][1]=j514_biquad(0,2000,rate,q);
        d->woofer[ch][2]=j514_biquad(1,100,rate,q);
        d->tweeter[ch][0]=j514_biquad(1,2000,rate,q);d->tweeter[ch][1]=j514_biquad(1,2000,rate,q);
    }
}
static inline void j514_dsp_frame(struct J514Dsp *d,const double in[2],double out[6])
{
    double woofer[2],tweeter[2];
    for(int ch=0;ch<2;++ch) {
        double w=in[ch];for(int k=0;k<3;++k)w=j514_run(&d->woofer[ch][k],w);
        double t=in[ch];for(int k=0;k<2;++k)t=j514_run(&d->tweeter[ch][k],t);
        woofer[ch]=w;tweeter[ch]=t;
    }
    out[0]=woofer[0];out[1]=woofer[1];out[2]=tweeter[0];out[3]=tweeter[1];out[4]=woofer[0];out[5]=woofer[1];
}
