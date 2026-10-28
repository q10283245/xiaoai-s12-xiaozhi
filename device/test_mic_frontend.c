/* Synthetic PCM only. No ALSA, socket, serial, credential or recording I/O. */
#include <stdio.h>
#include <stdlib.h>
#include "mic_frontend.h"

static int32_t raw[2880*8];
static int16_t pcm[6][960];
static uint64_t tick;
struct fixture {double noise[8],speech[8],frequency[8],dc[8];};
static void input(const struct fixture *f,unsigned frames) {
    for(unsigned i=0;i<frames;++i)for(unsigned ch=0;ch<8;++ch) {
        double t=(double)(tick+i)/48000.0;
        double x=f->dc[ch]+f->noise[ch]*sin(2*PI*(211+ch*31)*t)+
            f->speech[ch]*sin(2*PI*(f->frequency[ch]?f->frequency[ch]:997)*t);
        x*=65536;if(x>INT32_MAX)x=INT32_MAX;if(x<INT32_MIN)x=INT32_MIN;
        raw[i*8+ch]=(int32_t)lrint(x);
    }
    tick+=frames;
}
static void block(struct mic_frontend *m,const struct fixture *f) {
    input(f,2880);
    if(mic_frontend_process(m,raw,2880,30,pcm)!=960){fputs("sample count\n",stderr);exit(1);}
}
#define CHECK(x,message) do {if(!(x)){fprintf(stderr,"%s\n",message);return 1;}}while(0)
static int selection(void) {
    struct mic_frontend m;mic_frontend_init(&m,MIC_AUTO);
    struct fixture f={.noise={0,10,8,8000,8,10,0,0},.dc={0,4000,4000,4000,4000,4000,0,0}};
    for(unsigned i=0;i<12;++i)block(&m,&f);
    CHECK(!m.healthy[0],"dead channel accepted");
    f.speech[1]=15;f.speech[2]=300;f.speech[3]=1000;f.speech[4]=-300;f.speech[5]=60000;
    f.speech[6]=30000;f.speech[7]=-30000;
    for(unsigned i=0;i<3;++i)block(&m,&f);
    CHECK(m.selected==2||m.selected==4,"clean, audible channel not selected over weak/noisy/dead channels");
    CHECK(!m.healthy[5]&&m.clip[5]>.005,"clipped channel not rejected");
    CHECK(mic_rms(pcm[m.selected],960)>200,"opposite microphone phases cancelled the output");
    unsigned picked=m.selected;m.locked=1;
    f.speech[2]=picked==2?50:1800;f.speech[4]=picked==4?-50:-1800;
    for(unsigned i=0;i<12;++i)block(&m,&f);
    CHECK(m.selected==picked,"utterance changed microphone after lock");
    /* Even a new cable failure may not silently splice a different signal
     * into an accepted sentence. The next sentence can recover. */
    f.noise[picked]=0;f.dc[picked]=0;f.speech[picked]=0;
    for(unsigned i=0;i<4;++i)block(&m,&f);
    CHECK(m.selected==picked&&!m.healthy[picked],"failed locked channel changed unexpectedly");
    mic_frontend_reopen(&m);
    block(&m,&f);
    CHECK(m.selected!=picked,"next utterance failed to recover from dead mic");
    CHECK(m.noise[3]>4000,"strong stationary noise was learned as quiet");
    return 0;
}
static int hysteresis(void) {
    struct mic_frontend m;mic_frontend_init(&m,MIC_AUTO);
    struct fixture f={.noise={10,10,10,10,10,10,0,0}};
    for(unsigned i=0;i<12;++i)block(&m,&f);
    unsigned original=m.selected;
    /* Small gain differences do not rotate the selected mic every 60 ms. */
    for(unsigned i=0;i<40;++i) {
        for(unsigned ch=0;ch<6;++ch)f.speech[ch]=ch==(i%6)?4:0;
        block(&m,&f);CHECK(m.selected==original,"idle microphone chatter");
    }
    /* Preferred microphone and per-channel ambient floors persist across
     * capture reopen; source direction may change at the next boundary. */
    f.speech[5]=700;
    for(unsigned i=0;i<3;++i)block(&m,&f);
    CHECK(m.selected==5,"new direction did not select best microphone");
    m.locked=1;double noise=m.noise[5];mic_frontend_reopen(&m);
    CHECK(m.selected==5&&m.noise[5]==noise&&!m.locked,"reopen lost learned channel state");
    return 0;
}
static double filter_rms(unsigned mode,double frequency) {
    struct mic_frontend m;mic_frontend_init(&m,mode);
    struct fixture f={0};for(unsigned ch=0;ch<6;++ch){f.speech[ch]=8192;f.frequency[ch]=frequency;}
    double energy=0;
    for(unsigned i=0;i<6;++i){block(&m,&f);if(i>0){double r=mic_rms(pcm[m.selected],960);energy+=r*r;}}
    return sqrt(energy/5);
}
static int antialias(void) {
    double pass=filter_rms(MIC_AUTO,1000),stop=filter_rms(MIC_AUTO,12000);
    CHECK(pass>5700&&pass<5900,"array audible pass-band altered");
    CHECK(stop<pass*.01,"array aliases 12 kHz to false 4 kHz speech");
    CHECK(filter_rms(4,1000)>5700,"fixed-channel fallback changed");
    struct mic_frontend a,b;mic_frontend_init(&a,2);mic_frontend_init(&b,2);
    struct fixture f={.speech={0,0,5000},.frequency={0,0,1700}};
    tick=0;input(&f,2880);int32_t copy[2880*8];memcpy(copy,raw,sizeof(raw));
    int16_t expected[6][960],actual[6][960];
    CHECK(mic_frontend_process(&a,copy,2880,30,expected)==960,"full FIR block size");
    CHECK(mic_frontend_process(&b,copy,963,30,actual)==321,"partial FIR block size");
    int16_t tail[6][960];
    CHECK(mic_frontend_process(&b,copy+963*8,1917,30,tail)==639,"FIR continuation size");
    CHECK(!memcmp(expected[2],actual[2],321*2)&&!memcmp(expected[2]+321,tail[2],639*2),"FIR state lost at arbitrary frame boundary");
    CHECK(mic_frontend_process(&a,copy,2881,30,actual)==0,"oversize frame accepted");
    CHECK(mic_frontend_process(&a,copy,1,30,actual)==0,"non-divisible frame accepted");
    return 0;
}
int main(void) {
    if(selection()||hysteresis()||antialias())return 1;
    puts("{\"synthetic_six_mic_frontend\":\"passed\",\"room_recordings\":false}");return 0;
}
