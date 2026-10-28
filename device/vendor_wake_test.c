#define _POSIX_C_SOURCE 200809L
/* Native offline test fixture, never linked into a target/ARM executable.
 * The real model/ABI are validated by the separate bounded target probes. */
#if !defined(XIAOZHI_TEST_FAKE_CAPTURE) || defined(__arm__)
#error This deterministic fixture is for native offline tests only
#endif
#include "vendor_wake.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
struct s12_vendor_wake { uint64_t frames,next_hit; unsigned id,reference; };
static unsigned s12_test_reference_hits,s12_test_reference_opens;
struct s12_vendor_frontend { int16_t pending[288],delay[3200]; unsigned used,delay_samples,delay_at,epoch; uint64_t samples; int mode; };
s12_vendor_wake *s12_vendor_wake_open(char *error,size_t bytes) {
    static unsigned opens;
    (void)error;(void)bytes;s12_vendor_wake *w=calloc(1,sizeof(s12_vendor_wake));
    if(w){w->id=opens++%2;const char *trigger=getenv("S12_TEST_LOCAL_WAKE_FRAME");w->next_hit=trigger?strtoull(trigger,NULL,10):0;}
    return w;
}
void s12_test_wake_role(s12_vendor_wake *w,unsigned role){w->id=role;}
void s12_test_reference_wake(s12_vendor_wake *w) {
    w->reference=1;++s12_test_reference_opens;const char *trigger=getenv("S12_TEST_REFERENCE_WAKE_FRAME");w->next_hit=trigger?strtoull(trigger,NULL,10):0;
}
int s12_vendor_wake_process_event(s12_vendor_wake *w,const int16_t pcm[160],struct s12_vendor_wake_event *event) {
    (void)pcm;++w->frames;
    const char *reference_fault=getenv("S12_TEST_REFERENCE_FAULT_FRAME");
    if(w->reference&&reference_fault&&w->frames>=strtoull(reference_fault,NULL,10))return -1;
    static int stalled;
    const char *stall_frame=getenv("S12_TEST_KWS_STALL_FRAME"),*stall_ms=getenv("S12_TEST_KWS_STALL_MS");
    if(!stalled&&w->id==1&&stall_frame&&stall_ms&&w->frames==strtoull(stall_frame,NULL,10)) {
        stalled=1;unsigned ms=(unsigned)strtoul(stall_ms,NULL,10);if(ms>4000)ms=4000;
        struct timespec delay={ms/1000,(ms%1000)*1000000};(void)nanosleep(&delay,NULL);
    }
    memset(event,0,sizeof(*event));event->input_frames=w->frames;
    const char *source=getenv("S12_TEST_LOCAL_WAKE_SOURCE");
    if(!w->reference&&source&&!strcmp(source,"beam")&&w->id==1)return 0;
    const char *minimum=getenv(w->reference?"S12_TEST_REFERENCE_MIN_RMS":"S12_TEST_LOCAL_WAKE_MIN_RMS");
    double energy=0;if(minimum)for(unsigned i=0;i<160;++i)energy+=(double)pcm[i]*pcm[i];
    int audible=!minimum||sqrt(energy/160)>=strtod(minimum,NULL);
    const char *limit=getenv("S12_TEST_REFERENCE_HIT_COUNT");
    if(w->reference&&limit&&s12_test_reference_hits>=strtoul(limit,NULL,10))audible=0;
    if(w->next_hit&&w->frames>=w->next_hit&&audible) {
        const char *end=getenv(w->reference?"S12_TEST_REFERENCE_END_FRAME":"S12_TEST_LOCAL_WAKE_END_FRAME"),*begin=getenv(w->reference?"S12_TEST_REFERENCE_BEGIN_FRAME":"S12_TEST_LOCAL_WAKE_BEGIN_FRAME");
        const char *first=getenv(w->reference?"S12_TEST_REFERENCE_WAKE_FRAME":"S12_TEST_LOCAL_WAKE_FRAME");uint64_t base=w->next_hit-(first?strtoull(first,NULL,10):w->next_hit);
        event->end_frame=base+(end?strtoull(end,NULL,10):w->next_hit-base);
        event->begin_frame=base+(begin?strtoull(begin,NULL,10):event->end_frame-base>20?event->end_frame-base-20:0);
        event->status=1;event->timing_valid=event->begin_frame<event->end_frame&&event->end_frame<=event->input_frames;
        const char *invalid=getenv("S12_TEST_LOCAL_TIMING_INVALID");if(invalid)event->timing_valid=0;
        const char *period=getenv(w->reference?"S12_TEST_REFERENCE_WAKE_EVERY":"S12_TEST_LOCAL_WAKE_EVERY");w->next_hit=period?w->next_hit+strtoull(period,NULL,10):0;
        if(w->reference)++s12_test_reference_hits;
        return 1;
    }return 0;
}
int s12_vendor_wake_process(s12_vendor_wake *w,const int16_t pcm[160]) {
    struct s12_vendor_wake_event event;return s12_vendor_wake_process_event(w,pcm,&event);
}
const char *s12_vendor_wake_version(s12_vendor_wake *w){(void)w;return "offline-fixture";}
const char *s12_vendor_wake_error(s12_vendor_wake *w){(void)w;return "fixture failure";}
void s12_vendor_wake_close(s12_vendor_wake *w){free(w);}
s12_vendor_frontend *s12_vendor_frontend_open(s12_vendor_wake *w,char *e,size_t n){
    (void)w;(void)e;(void)n;
    const char *mode=getenv("S12_TEST_AFE");if(!mode||!strcmp(mode,"unavailable"))return NULL;
    static unsigned attempts;unsigned attempt=attempts++;
    if((!strcmp(mode,"recover-available")&&!attempt)||(!strcmp(mode,"fail-after-prior")&&attempt))return NULL;
    s12_vendor_frontend *f=calloc(1,sizeof(*f));if(!f)return NULL;
    static unsigned opens;f->epoch=opens++;
    f->mode=!strcmp(mode,"weak")?1:!strcmp(mode,"clipped")?2:!strcmp(mode,"broken")?3:
        !strcmp(mode,"cue-trace")||!strcmp(mode,"fail-after-prior")?4:!strcmp(mode,"ambient-weak")?5:
        !strcmp(mode,"startup-ambient")?6:!strcmp(mode,"source-switch")?7:0;
    const char *delay=getenv("S12_TEST_AFE_DELAY_FRAMES");f->delay_samples=delay?(unsigned)strtoul(delay,NULL,10)*160:0;
    if(f->delay_samples>3200)f->delay_samples=3200;
    return f;
}
int s12_vendor_frontend_process(s12_vendor_frontend *f,const int16_t pcm[6][128],int16_t out[160]){
    if(f->mode==3)return -1;
    for(unsigned i=0;i<128;++i){
        int16_t value=f->mode==1?pcm[2][i]/100:f->mode==2?32767:pcm[2][i];
        if(f->mode>=4) {
            double t=(double)f->samples/16000,rms=35;
            if(f->mode==4&&(f->epoch&1)) {
                const double trace[]={2319,17,20,28,42.8,89.3,62.5,57,42,35,28,66,40,32,48,58,37,44,33,48};
                unsigned block=(unsigned)(f->samples/960);rms=trace[block<20?block:19];
            }
            if(f->mode==6)rms=t<.8?194:45;
            if(f->mode==7&&(f->epoch&1)&&t>=.18)rms=200;
            double v=rms*1.41421356237*sin(6.28318530718*311*t);
            const char *duration=getenv("S12_TEST_COMMAND_DURATION_MS"),*begin=getenv("S12_TEST_BF_COMMAND_BEGIN_MS");
            double start=begin?strtod(begin,NULL)/1000:0,end=start+(duration?strtod(duration,NULL)/1000:.3);
            if(f->mode==5&&(f->epoch&1)&&t>=start&&t<end)v+=180*1.41421356237*sin(6.28318530718*997*t);
            value=(int16_t)lrint(v);
            if(f->mode==7&&(f->epoch&1)&&t>=.3)value=32767;
        }
        ++f->samples;
        if(f->delay_samples){int16_t old=f->delay[f->delay_at];f->delay[f->delay_at]=value;f->delay_at=(f->delay_at+1)%f->delay_samples;value=old;}
        f->pending[f->used++]=value;
    }
    if(f->used<160)return 0;
    memcpy(out,f->pending,320);f->used-=160;memmove(f->pending,f->pending+160,f->used*2);return 1;
}
const char *s12_vendor_frontend_error(s12_vendor_frontend *f){(void)f;return "fixture failure";}
void s12_vendor_frontend_close(s12_vendor_frontend *f){free(f);}
/* Six-channel AEC fixture. Production never links this file. All effects are
 * synthetic RAM samples; tests can fail a complete block without ever
 * exposing partial output to the client's keyword detectors. */
struct s12_vendor_aec { unsigned blocks;int failed; };
s12_vendor_aec *s12_vendor_aec_open(s12_vendor_wake *w,char *e,size_t n) {
    (void)w;(void)e;(void)n;
    const char *fault=getenv("S12_TEST_AEC_FAULT");
    if(fault&&!strcmp(fault,"open"))return NULL;
    return calloc(1,sizeof(s12_vendor_aec));
}
int s12_vendor_aec_process(s12_vendor_aec *a,const int16_t mic[6][128],const int16_t ref[128],int16_t out[6][128]) {
    const char *fault=getenv("S12_TEST_AEC_FAULT");
    ++a->blocks;
    const char *after=getenv("S12_TEST_AEC_FAIL_AFTER");unsigned fail_after=after?(unsigned)strtoul(after,NULL,10):2;
    if(a->failed||(fault&&!strcmp(fault,"process")&&a->blocks>=fail_after)){a->failed=1;return -1;}
    const char *mode=getenv("S12_TEST_AEC_MODE");
    for(unsigned ch=0;ch<6;++ch)for(unsigned i=0;i<128;++i) {
        int value=mic[ch][i];if(mode&&!strcmp(mode,"subtract"))value-=ref[i];
        if(value>32767)value=32767;if(value<-32768)value=-32768;out[ch][i]=(int16_t)value;
    }
    return 0;
}
int s12_vendor_aec_reset(s12_vendor_aec *a){a->blocks=0;a->failed=0;return 0;}
const char *s12_vendor_aec_error(s12_vendor_aec *a){(void)a;return "fixture failure";}
int s12_vendor_aec_close(s12_vendor_aec *a){free(a);return 0;}
