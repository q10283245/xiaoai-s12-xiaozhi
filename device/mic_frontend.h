/* S12 six-microphone diversity frontend. No geometry/phase calibration is
 * assumed: select one complete channel, never average unrelated waveforms.
 * All six FIRs stay warm; selection is frozen at utterance onset. */
#ifndef S12_MIC_FRONTEND_H
#define S12_MIC_FRONTEND_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifndef FILTER_TAPS
#define FILTER_TAPS 63u
#endif
#ifndef CAPTURE_CHANNELS
#define CAPTURE_CHANNELS 8u
#endif
#ifndef UPLINK_SAMPLES
#define UPLINK_SAMPLES 960u
#endif
#ifndef PI
#define PI 3.14159265358979323846
#endif
#define MIC_CHANNELS 6u
#define MIC_AUTO MIC_CHANNELS

/* Linear-phase Hamming-windowed sinc, 6.4 kHz cutoff at 48 kHz. State
 * survives packet boundaries; channels 6/7 are never microphone candidates. */
struct decimator {double taps[FILTER_TAPS],history[FILTER_TAPS];unsigned pos,phase;};
static void decimator_init(struct decimator *d) {
    memset(d,0,sizeof(*d));double sum=0,fc=6400.0/48000.0;
    for(unsigned i=0;i<FILTER_TAPS;++i) {
        double x=(double)i-(FILTER_TAPS-1)/2.0;
        double sinc=x==0?2*fc:sin(2*PI*fc*x)/(PI*x);
        d->taps[i]=sinc*(0.54-0.46*cos(2*PI*i/(FILTER_TAPS-1)));
        sum+=d->taps[i];
    }
    for(unsigned i=0;i<FILTER_TAPS;++i)d->taps[i]/=sum;
}
static unsigned decimate(struct decimator *d,const int32_t *interleaved,unsigned frames,unsigned channel,int16_t *out) {
    unsigned produced=0;
    for(unsigned i=0;i<frames;++i) {
        d->history[d->pos]=(double)interleaved[i*CAPTURE_CHANNELS+channel]/65536.0;
        d->pos=(d->pos+1)%FILTER_TAPS;
        if(++d->phase!=3)continue;
        d->phase=0;double y=0;
        for(unsigned k=0;k<FILTER_TAPS;++k)y+=d->taps[k]*d->history[(d->pos+FILTER_TAPS-1-k)%FILTER_TAPS];
        if(y>32767)y=32767;if(y<-32768)y=-32768;
        out[produced++]=(int16_t)lrint(y);
    }
    return produced;
}

struct mic_frontend {
    struct decimator filters[MIC_CHANNELS];
    double noise[MIC_CHANNELS],rms[MIC_CHANNELS],clip[MIC_CHANNELS],score[MIC_CHANNELS];
    unsigned char initialized[MIC_CHANNELS],healthy[MIC_CHANNELS];
    unsigned mode,selected,challenger,challenge_frames,blocks,locked;
};
static void mic_frontend_reopen(struct mic_frontend *m) {
    /* Noise and preferred microphone survive half-duplex close/play/reopen.
     * FIR histories cannot: playback gaps are not contiguous capture audio. */
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)decimator_init(&m->filters[ch]);
    m->blocks=0;m->locked=0;m->challenge_frames=0;
}
static void mic_frontend_init(struct mic_frontend *m,unsigned mode) {
    memset(m,0,sizeof(*m));m->mode=mode;m->selected=mode==MIC_AUTO?0:mode;
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch)m->noise[ch]=8;
    mic_frontend_reopen(m);
}
static double mic_rms(const int16_t *pcm,unsigned n) {
    if(!n)return 0;
    double sum=0,energy=0;
    for(unsigned i=0;i<n;++i)sum+=pcm[i];
    double mean=sum/n;
    for(unsigned i=0;i<n;++i){double x=pcm[i]-mean;energy+=x*x;}
    return sqrt(energy/n);
}
static unsigned mic_frontend_process(struct mic_frontend *m,const int32_t *raw,unsigned frames,
                                     unsigned min_rms,int16_t pcm[MIC_CHANNELS][UPLINK_SAMPLES]) {
    if(!frames||frames>UPLINK_SAMPLES*3||frames%3||m->mode>MIC_AUTO)return 0;
    if(m->mode!=MIC_AUTO)return decimate(&m->filters[m->selected],raw,frames,m->selected,pcm[m->selected]);
    unsigned n=frames/3,best=m->selected;double best_score=-1;
    for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
        if(decimate(&m->filters[ch],raw,frames,ch,pcm[ch])!=n)return 0;
        /* Exclude only the first FIR settling transient from the metrics,
         * never from the outgoing PCM or its exact sample count. */
        unsigned skip=!m->blocks&&n>32?32:0;
        m->rms[ch]=mic_rms(pcm[ch]+skip,n-skip);
        unsigned clipped=0;
        for(unsigned i=0;i<frames;++i) {
            int32_t x=raw[i*CAPTURE_CHANNELS+ch];
            if(x>2115271393||x< -2115271393)++clipped; /* 98.5% full scale. */
        }
        m->clip[ch]=(double)clipped/frames;
        m->healthy[ch]=(unsigned char)(m->rms[ch]>=1&&m->clip[ch]<=0.005);
        if(!m->initialized[ch]) {
            m->noise[ch]=m->rms[ch]<2?2:m->rms[ch]>12000?12000:m->rms[ch];
            m->initialized[ch]=1;
        }
        double floor=m->noise[ch],amplitude_floor=(double)min_rms/3.5;
        if(floor<amplitude_floor)floor=amplitude_floor;
        m->score[ch]=m->healthy[ch]?m->rms[ch]/floor:0;
        if(m->score[ch]>best_score){best=ch;best_score=m->score[ch];}
    }
    if(!m->locked) {
        /* A clear speech arrival can select its best microphone immediately,
         * before the 180 ms VAD onset. Small idle differences require three
         * frames and a 35% margin. A locked utterance never changes channel,
         * even if a second source becomes louder or its chosen mic fails. */
        double current=m->score[m->selected];
        if(best!=m->selected&&m->healthy[best]&&
           (!m->healthy[m->selected]||best_score>current*1.35)) {
            if(m->challenger!=best){m->challenger=best;m->challenge_frames=1;}
            else ++m->challenge_frames;
            if(!m->healthy[m->selected]||best_score>=3.5||m->challenge_frames>=3) {
                m->selected=best;m->challenge_frames=0;
            }
        } else m->challenge_frames=0;
        /* Learn ambient noise only when no channel has a plausible onset.
         * This keeps the quiet/far microphone's floor from learning speech.
         * Fast downward recovery also handles a noisy first power-on frame. */
        if(best_score<2.5)for(unsigned ch=0;ch<MIC_CHANNELS;++ch) {
            double sample=m->rms[ch];if(sample<2)sample=2;if(sample>12000)sample=12000;
            double alpha=sample<m->noise[ch]?0.10:0.015;
            m->noise[ch]+=alpha*(sample-m->noise[ch]);
        }
    }
    ++m->blocks;return n;
}
#endif
