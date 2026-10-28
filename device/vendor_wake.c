#define _POSIX_C_SOURCE 200809L
#include "vendor_wake.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Original ARM wrapper context is two pointers: task, PaddyEngine. The result
 * returned by WakeupProcess uses an explicit ARM aggregate-sret destination:
 * result[0] is the real hit flag, as checked by xaudio_wakeup_thread. Extra
 * fields/diagnostic words are opaque. Keep guards around the exact 576 bytes. */
typedef int (*init_fn)(void *, const char *, const char *);
typedef void *(*process_fn)(void *, void *, const int16_t *, unsigned);
typedef const char *(*version_fn)(void *);
typedef int (*release_fn)(void *);
typedef void *(*afe_create_fn)(const char *, unsigned);
typedef int (*afe_process_fn)(void *, const int16_t *, unsigned, int16_t *, unsigned *, unsigned, float *);
typedef int (*afe_set_fn)(void *, unsigned, unsigned);
typedef int (*afe_destroy_fn)(void **);
typedef void *(*aec_create_fn)(void);
typedef int (*aec_process_fn)(void *, const int16_t *, const int16_t *, unsigned, int16_t *, unsigned *);
typedef int (*aec_set_fn)(void *, unsigned, unsigned);
typedef int (*aec_destroy_fn)(void **);
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
typedef int (*aec_preprocess_ctl_fn)(void *,int,void *);
extern void s12_debug_aec_params(const struct s12_aec_debug_params *);
static unsigned s12_aec_params_epoch;
#endif
struct s12_vendor_aec {
    void *context[6];
    aec_create_fn create;
    aec_process_fn process;
    aec_set_fn set;
    aec_destroy_fn destroy;
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    aec_preprocess_ctl_fn debug_ctl;
    struct s12_aec_debug_params debug_params;
#endif
    char error[128];
    int failed;
};
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AFE_OBSERVE) && S12_DEBUG_AFE_OBSERVE
extern void s12_debug_afe_observe(unsigned,uint64_t,uint64_t,const int16_t *,int);
static unsigned s12_debug_afe_epochs;
#endif
struct s12_vendor_frontend {
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AFE_OBSERVE) && S12_DEBUG_AFE_OBSERVE
    unsigned debug_epoch;uint64_t debug_input_samples,debug_output_samples;
#endif
    void *context;
    afe_process_fn process;
    afe_destroy_fn destroy;
    char error[128];
};
struct s12_vendor_wake {
    void *libraries[5];
    void *context[2];
    init_fn init;
    process_fn process;
    version_fn version;
    release_fn release;
    char model[64];
    char error[256];
    int initialized;
    uint64_t input_frames;
#ifdef S12_VENDOR_WAKE_DIAGNOSTICS
    struct s12_vendor_wake_diagnostics diagnostics;
#endif
};

static void error_set(char *out, size_t n, const char *text) {
    if (out && n) snprintf(out, n, "%s", text ? text : "vendor wake failure");
}
static void *required(void *lib, const char *name) {
    dlerror();
    return dlsym(lib, name);
}

s12_vendor_wake *s12_vendor_wake_open(char *error, size_t n) {
    static const char *paths[] = {
        "/run/s12/system0/lib/libpthread.so.0",
        "/run/s12/system0/lib/libm.so.6",
        "/run/s12/system0/usr/lib/libstdc++.so.6",
        "/run/s12/system0/usr/lib/libasound.so.2",
        "/run/s12/system0/usr/lib/libxaudio_engine.so"
    };
    s12_vendor_wake *s = calloc(1, sizeof(*s));
    if (!s) { error_set(error, n, "allocation failed"); return NULL; }
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        s->libraries[i] = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (!s->libraries[i]) {
            error_set(error, n, dlerror());
            s12_vendor_wake_close(s);
            return NULL;
        }
    }
    void *lib = s->libraries[4];
    *(void **)(&s->init) = required(lib, "xaudio_wrapper_wakeup_init");
    *(void **)(&s->process) = required(lib, "xaudio_wrapper_wakeup");
    *(void **)(&s->version) = required(lib, "xaudio_wrapper_get_wakeupModelVersion");
    *(void **)(&s->release) = required(lib, "xaudio_wrapper_wakeup_release");
    if (!s->init || !s->process || !s->version || !s->release) {
        error_set(error, n, "required S12A wrapper export missing");
        s12_vendor_wake_close(s);
        return NULL;
    }
    /* The exact binary retains this path as a label. RiceLoadModelBin copies
     * two embedded BINN models from its .rodata; it does not open this file. */
    if (s->init(s->context, "/usr/share/xiaomi/wakeup_model.bin", "") != 0 ||
        !s->context[0] || !s->context[1]) {
        error_set(error, n, "vendor wake init failed");
        s12_vendor_wake_close(s);
        return NULL;
    }
    s->initialized = 1;
    const char *model = s->version(s->context);
    if (!model || strnlen(model, sizeof(s->model)) >= sizeof(s->model) ||
        strcmp(model, "s12a_lx06_20211206")) {
        error_set(error, n, "unexpected embedded wake model version");
        s12_vendor_wake_close(s);
        return NULL;
    }
    snprintf(s->model, sizeof(s->model), "%s", model);
    return s;
}

int s12_vendor_wake_process_event(s12_vendor_wake *s, const int16_t pcm[160],struct s12_vendor_wake_event *event) {
    struct {
        uint32_t before[8];
        uint64_t result[72];
        uint32_t after[8];
    } output;
    if(event)memset(event,0,sizeof(*event));
    if (!s || !s->initialized || !pcm) return -1;
#if defined(S12_VENDOR_WAKE_DIAGNOSTICS) && defined(__arm__) && UINTPTR_MAX == UINT32_MAX
    unsigned char *trace_impl = NULL;
    uint32_t trace_before_status, trace_before_active;
    uint64_t trace_before_begin, trace_before_end;
    memcpy(&trace_impl, s->context[1], sizeof(trace_impl));
    memcpy(&trace_before_status, trace_impl + 20, 4);
    memcpy(&trace_before_active, trace_impl + 116, 4);
    memcpy(&trace_before_begin, trace_impl + 120, 8);
    memcpy(&trace_before_end, trace_impl + 128, 8);
#endif
    for (unsigned i = 0; i < 8; ++i) output.before[i] = output.after[i] = 0x5a31ac79u;
    memset(output.result, 0, sizeof(output.result));
    /* RiceTFCnnProcess divides this argument by two before reading int16 PCM;
     * it is a byte count, not a sample count. Original replay block is 320 B. */
    s->process(output.result, s->context, pcm, 320);
    ++s->input_frames;
    for (unsigned i = 0; i < 8; ++i)
        if (output.before[i] != 0x5a31ac79u || output.after[i] != 0x5a31ac79u) {
            snprintf(s->error, sizeof(s->error), "guard[%u] before=%08x after=%08x", i,
                     output.before[i], output.after[i]);
            return -1;
        }
    uint32_t hit;
    memcpy(&hit, output.result, sizeof(hit));
    /* Exact Paddy impl 0x5da2c assigns status 2 when its level-2 window
     * expires without a match; original xaudio_wakeup_thread routes 2 to a
     * suspect/timeout callback (0x27f38), never its real-hit branch. Keep that
     * status distinct from 1; reject all other values until corroborated. */
    if (hit != 0 && hit != 1 && hit != 2) {
        uint32_t words[8]; memcpy(words, output.result, sizeof(words));
        snprintf(s->error, sizeof(s->error), "hit/status=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x", hit,
                 words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7]);
        return -1;
    }
    if(event) {
        event->input_frames=s->input_frames;event->status=hit;
        if(hit==1) {
            /* WakeupProcess 0x32874/0x32878 loads Paddy result +72/+80
             * (PaddyImpl +88/+96), then 0x32894/0x32898 writes these exact
             * 64-bit values to public result +560/+568. This does not read
             * private object offsets or infer a boundary from audio energy. */
            memcpy(&event->begin_frame,(unsigned char *)output.result+560,8);
            memcpy(&event->end_frame,(unsigned char *)output.result+568,8);
            event->timing_valid=event->begin_frame<event->end_frame&&event->end_frame<=event->input_frames;
        }
    }
#ifdef S12_VENDOR_WAKE_DIAGNOSTICS
    ++s->diagnostics.status[hit];
    /* Read-only, exact ARM32 private layout for the bounded replay probe.
     * Original Paddy object -> impl -> L1 Rice engine; these offsets come
     * from 0x5d5f0 / RiceTFCnnProcess 0x603b4..0x60408. No values are changed. */
#if defined(__arm__) && UINTPTR_MAX == UINT32_MAX
    unsigned char *impl = NULL, *rice = NULL;
    memcpy(&impl, s->context[1], sizeof(impl));
    memcpy(&rice, impl + 8, sizeof(rice));
    memcpy(&s->diagnostics.l1_score, rice + 0x664, 4);
    memcpy(&s->diagnostics.l1_threshold, rice + 0x7c0, 4);
    memcpy(&s->diagnostics.l1_frames, rice + 0x320, 4);
    memcpy(&s->diagnostics.l1_warmup, rice + 0x7b8, 4);
    s->diagnostics.l1_flag = rice[0x624];
    if (s->diagnostics.l1_score > s->diagnostics.l1_max_score)
        s->diagnostics.l1_max_score = s->diagnostics.l1_score;
    uint32_t impl_status, active, words[4];
    uint64_t begin, end, previous_begin, previous_end;
    memcpy(&impl_status, impl + 20, 4);
    memcpy(&active, impl + 116, 4);
    memcpy(&begin, impl + 88, 8);
    memcpy(&end, impl + 96, 8);
    s->diagnostics.result_begin_frame=begin;s->diagnostics.result_end_frame=end;
    memcpy(&previous_begin, impl + 120, 8);
    memcpy(&previous_end, impl + 128, 8);
    /* Copy only locals already read by this established diagnostics path;
     * no new OEM offset is read, written or interpreted as an API. */
    s->diagnostics.impl_status=impl_status;s->diagnostics.active=active;
    s->diagnostics.previous_begin_frame=previous_begin;s->diagnostics.previous_end_frame=previous_end;
    memcpy(words, output.result, sizeof(words));
#if defined(S12_DEBUG_MODEL_DIAGNOSTICS) && S12_DEBUG_MODEL_DIAGNOSTICS
    /* Runtime snapshot replaces verbose per-frame standalone replay output. */
    if (0 && (hit || impl_status || previous_begin != trace_before_begin || previous_end != trace_before_end))
#else
    if (hit || impl_status || previous_begin != trace_before_begin || previous_end != trace_before_end)
#endif
        printf("{\"trace\":true,\"frame\":%u,\"status\":%u,\"impl_status\":%u,\"before_status\":%u,\"active\":%u,\"before_active\":%u,\"begin\":%" PRIu64 ",\"end\":%" PRIu64 ",\"previous_begin\":%" PRIu64 ",\"previous_end\":%" PRIu64 ",\"before_previous_begin\":%" PRIu64 ",\"before_previous_end\":%" PRIu64 ",\"words\":[%u,%u,%u,%u]}\n",
               s->diagnostics.l1_frames, hit, impl_status, trace_before_status, active, trace_before_active,
               begin, end, previous_begin, previous_end, trace_before_begin, trace_before_end,
               words[0], words[1], words[2], words[3]);
#endif
#endif
    return hit == 1;
}
int s12_vendor_wake_process(s12_vendor_wake *s,const int16_t pcm[160]) {
    return s12_vendor_wake_process_event(s,pcm,NULL);
}

const char *s12_vendor_wake_version(s12_vendor_wake *s) { return s ? s->model : ""; }
const char *s12_vendor_wake_error(s12_vendor_wake *s) { return s ? s->error : "null adapter"; }
#ifdef S12_VENDOR_WAKE_DIAGNOSTICS
void s12_vendor_wake_diagnostics(s12_vendor_wake *s, struct s12_vendor_wake_diagnostics *d) {
    if (s && d) *d = s->diagnostics;
}
#endif
void s12_vendor_wake_close(s12_vendor_wake *s) {
    if (!s) return;
    if (s->initialized && s->release) s->release(s->context);
    for (unsigned i = 5; i; --i) if (s->libraries[i - 1]) dlclose(s->libraries[i - 1]);
    memset(s, 0, sizeof(*s));
    free(s);
}

s12_vendor_frontend *s12_vendor_frontend_open(s12_vendor_wake *wake, char *error, size_t bytes) {
    if (!wake || !wake->initialized) { error_set(error, bytes, "wake library unavailable"); return NULL; }
    s12_vendor_frontend *f = calloc(1, sizeof(*f));
    if (!f) { error_set(error, bytes, "frontend allocation failed"); return NULL; }
    afe_create_fn create = NULL; afe_set_fn set = NULL;
    void *lib = wake->libraries[4];
    *(void **)(&create) = required(lib, "CreatAFEEnhancementHandler");
    *(void **)(&set) = required(lib, "SetAFEEnhancementMember");
    *(void **)(&f->process) = required(lib, "AFEEnhancementProcess");
    *(void **)(&f->destroy) = required(lib, "DestroyAFEEnhancementHandler");
    if (!create || !set || !f->process || !f->destroy) {
        error_set(error, bytes, "required S12A AFE export missing"); free(f); return NULL;
    }
    f->context = create("/run/s12/system0/usr/share/xiaomi/dnn_vad_model", 0);
    if (!f->context) { error_set(error, bytes, "S12A AFE initialization failed"); free(f); return NULL; }
    /* Match xaudio_bf_init. No AEC/playback reference is claimed by this API. */
    set(f->context, 0, 0); set(f->context, 0, 1);
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AFE_OBSERVE) && S12_DEBUG_AFE_OBSERVE
    f->debug_epoch=++s12_debug_afe_epochs;
#endif
    return f;
}
int s12_vendor_frontend_process(s12_vendor_frontend *f, const int16_t pcm[6][128], int16_t out[160]) {
    struct { uint32_t before[8]; int16_t pcm[320]; uint32_t after[8]; } output;
    if (!f || !f->context || !pcm || !out) return -1;
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AFE_OBSERVE) && S12_DEBUG_AFE_OBSERVE
    f->debug_input_samples+=128;
#endif
    for (unsigned i = 0; i < 8; ++i) output.before[i] = output.after[i] = 0x5a31ac79u;
    memset(output.pcm, 0, sizeof(output.pcm)); unsigned bytes = 0; float angle = 0;
    int r = f->process(f->context, &pcm[0][0], 1536, output.pcm, &bytes, 0, &angle);
    for (unsigned i = 0; i < 8; ++i) if (output.before[i] != 0x5a31ac79u || output.after[i] != 0x5a31ac79u) {
        error_set(f->error, sizeof(f->error), "S12A AFE output guard changed"); return -1;
    }
    if (!isfinite(angle) || angle < -360 || angle > 360 || !((r == 0 && bytes == 640) || (r == -1 && bytes == 0))) {
        error_set(f->error, sizeof(f->error), "S12A AFE output contract changed"); return -1;
    }
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AFE_OBSERVE) && S12_DEBUG_AFE_OBSERVE
    if(bytes)f->debug_output_samples+=160;
    s12_debug_afe_observe(f->debug_epoch,f->debug_input_samples,f->debug_output_samples,bytes?output.pcm:NULL,!!bytes);
#endif
    if (!bytes) return 0;
    memcpy(out, output.pcm, 320); return 1;
}
const char *s12_vendor_frontend_error(s12_vendor_frontend *f) { return f ? f->error : "null frontend"; }
void s12_vendor_frontend_close(s12_vendor_frontend *f) {
    if (!f) return;
    if (f->context && f->destroy) f->destroy(&f->context);
    memset(f, 0, sizeof(*f)); free(f);
}

/* The exact AECProcess at0x32b34 takes6 arguments. Its fourth, byte-count
 * argument is unused; the original wrapper passes1536 even though the Speex
 * state consumes one128-sample mic plane. Output is argument5 and the guarded
 * byte-count destination is argument6. This is NOT generic Speex's ABI.
 * DestroyAECHandler at0x32af4 takes void**, but clears only the first byte of
 * that pointer. Always clear our complete pointer ourselves after the call. */
#define AEC_GUARD UINT32_C(0xa3e15c7d)
struct aec_guarded_pcm { uint32_t before[8]; int16_t pcm[128]; uint32_t after[8]; };
static void aec_pcm_init(struct aec_guarded_pcm *p,const int16_t *source) {
    for(unsigned i=0;i<8;++i)p->before[i]=p->after[i]=AEC_GUARD;
    if(source)memcpy(p->pcm,source,256);else memset(p->pcm,0,256);
}
static int aec_pcm_guard(const struct aec_guarded_pcm *p) {
    for(unsigned i=0;i<8;++i)if(p->before[i]!=AEC_GUARD||p->after[i]!=AEC_GUARD)return -1;
    return 0;
}
static int aec_release_states(s12_vendor_aec *a) {
    int failed=0;
    for(unsigned ch=0;ch<6;++ch)if(a->context[ch]) {
        struct {uint32_t before[8];void *context;uint32_t after[8];} guarded;
        for(unsigned i=0;i<8;++i)guarded.before[i]=guarded.after[i]=AEC_GUARD;
        guarded.context=a->context[ch];
        int r=a->destroy(&guarded.context);
        a->context[ch]=NULL;
        for(unsigned i=0;i<8;++i)
            if(guarded.before[i]!=AEC_GUARD||guarded.after[i]!=AEC_GUARD)failed=1;
        if(r!=0)failed=1;
    }
    return failed?-1:0;
}
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
static int aec_debug_get_params(s12_vendor_aec *a,unsigned ch) {
    struct s12_aec_debug_channel_params *d=&a->debug_params.channel[ch];
    /* Exact ARM32 CreatAECHandler stores linear echo atctx+0 and preproc at
     * ctx+4. sizeof(void*) is4 ontheverifiedtarget; native mocks usehostwidth. */
    void *linear=NULL,*preprocess=NULL;memcpy(&linear,a->context[ch],sizeof(linear));memcpy(&preprocess,(unsigned char *)a->context[ch]+sizeof(void *),sizeof(preprocess));
    d->linear_present=linear!=NULL;d->preprocess_present=preprocess!=NULL;
    static const int requests[5]={1,19,21,23,25};
    int32_t *values[4]={&d->denoise,&d->noise_suppress,&d->echo_suppress,&d->echo_suppress_active};
    if(!a->debug_ctl||!preprocess)return -1;
    for(unsigned i=0;i<5;++i) {
        struct {uint32_t before[8];int32_t value;uint32_t after[8];} integer;
        struct {uint32_t before[8];void *value;uint32_t after[8];} pointer;
        for(unsigned g=0;g<8;++g)integer.before[g]=integer.after[g]=pointer.before[g]=pointer.after[g]=AEC_GUARD;
        integer.value=INT32_MIN;pointer.value=(void *)(uintptr_t)UINTPTR_MAX;
        d->status[i]=a->debug_ctl(preprocess,requests[i],i<4?(void *)&integer.value:(void *)&pointer.value);
        int guards=0;for(unsigned g=0;g<8;++g)if(integer.before[g]!=AEC_GUARD||integer.after[g]!=AEC_GUARD||pointer.before[g]!=AEC_GUARD||pointer.after[g]!=AEC_GUARD)guards=1;
        if(d->status[i]||guards)return -1;
        if(i<4)*values[i]=integer.value;else {d->connection_present=pointer.value!=NULL;d->connection_matches_linear=linear&&pointer.value==linear;}
    }
    d->valid=1;a->debug_params.valid_mask|=1u<<ch;return 0;
}
#endif
int s12_vendor_aec_reset(s12_vendor_aec *a) {
    if(!a)return -1;
    a->failed=1;
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    memset(&a->debug_params,0,sizeof(a->debug_params));a->debug_params.epoch=++s12_aec_params_epoch;
    for(unsigned ch=0;ch<6;++ch)for(unsigned k=0;k<5;++k)a->debug_params.channel[ch].status[k]=INT32_MIN;
#endif
    if(aec_release_states(a)) {error_set(a->error,sizeof(a->error),"AEC destructor ABI failure");return -1;}
    for(unsigned ch=0;ch<6;++ch) {
        a->context[ch]=a->create();
        /* Original xaudio_wrapper_aec sets member0 from its wakeup-active
         * flag. During wake detection the original flag is0. Do not expose
         * an inferred generic 'freeze' switch until separately validated. */
        if(!a->context[ch]||a->set(a->context[ch],0,0)!=0) {
            (void)aec_release_states(a);
            error_set(a->error,sizeof(a->error),"AEC initialization failed");return -1;
        }
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
        if(aec_debug_get_params(a,ch)){a->debug_params.error=1;a->debug_params.error_channel=ch;s12_debug_aec_params(&a->debug_params);(void)aec_release_states(a);error_set(a->error,sizeof(a->error),"AEC read-only parameter probe failed");return -1;}
#endif
    }
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    s12_debug_aec_params(&a->debug_params);
#endif
    a->failed=0;a->error[0]=0;return 0;
}
s12_vendor_aec *s12_vendor_aec_open(s12_vendor_wake *wake,char *error,size_t bytes) {
    if(!wake||!wake->initialized) {error_set(error,bytes,"wake library unavailable");return NULL;}
    s12_vendor_aec *a=calloc(1,sizeof(*a));
    if(!a) {error_set(error,bytes,"AEC allocation failed");return NULL;}
    void *lib=wake->libraries[4];
    *(void **)(&a->create)=required(lib,"CreatAECHandler");
    *(void **)(&a->process)=required(lib,"AECProcess");
    *(void **)(&a->set)=required(lib,"SetAECMember");
    *(void **)(&a->destroy)=required(lib,"DestroyAECHandler");
    if(!a->create||!a->process||!a->set||!a->destroy) {
        error_set(error,bytes,"required S12A AEC export missing");free(a);return NULL;
    }
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS && defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    *(void **)(&a->debug_ctl)=required(lib,"speex_preprocess_ctl");
    if(!a->debug_ctl){error_set(error,bytes,"AEC read-only ctl symbol missing");free(a);return NULL;}
#endif
    if(s12_vendor_aec_reset(a)) {
        error_set(error,bytes,a->error);(void)s12_vendor_aec_close(a);return NULL;
    }
    return a;
}
int s12_vendor_aec_process(s12_vendor_aec *a,const int16_t mic[6][128],const int16_t reference[128],int16_t clean[6][128]) {
    if(!a||!mic||!reference||!clean)return -1;
    if(a->failed) {error_set(a->error,sizeof(a->error),"AEC reset required");return -1;}
    struct aec_guarded_pcm input[6],output[6],ref;
    aec_pcm_init(&ref,reference);
    for(unsigned ch=0;ch<6;++ch) {aec_pcm_init(&input[ch],mic[ch]);aec_pcm_init(&output[ch],NULL);}
    for(unsigned ch=0;ch<6;++ch) {
        struct {uint32_t before[8];unsigned value;uint32_t after[8];} length;
        for(unsigned i=0;i<8;++i)length.before[i]=length.after[i]=AEC_GUARD;
        length.value=0;
        int r=a->context[ch]?a->set(a->context[ch],0,0):-1;
        if(!r)r=a->process(a->context[ch],input[ch].pcm,ref.pcm,1536,output[ch].pcm,&length.value);
        int failed=r!=0||length.value!=256||aec_pcm_guard(&ref)||memcmp(ref.pcm,reference,256);
        for(unsigned i=0;i<8;++i)
            if(length.before[i]!=AEC_GUARD||length.after[i]!=AEC_GUARD)failed=1;
        for(unsigned i=0;i<6;++i)
            if(aec_pcm_guard(&input[i])||aec_pcm_guard(&output[i])||memcmp(input[i].pcm,mic[i],256))failed=1;
        if(failed) {
            a->failed=1;error_set(a->error,sizeof(a->error),"AEC process ABI or input/output guard failure");return -1;
        }
    }
    for(unsigned ch=0;ch<6;++ch)memcpy(clean[ch],output[ch].pcm,256);
    return 0;
}
const char *s12_vendor_aec_error(s12_vendor_aec *a) {return a?a->error:"null AEC adapter";}
int s12_vendor_aec_close(s12_vendor_aec *a) {
    if(!a)return 0;
    int r=aec_release_states(a);memset(a,0,sizeof(*a));free(a);return r;
}
