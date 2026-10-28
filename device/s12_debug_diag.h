/* Private RAM debug build only. No endpoint or hooks in the canonical build. */
#ifndef S12_DEBUG_DIAG_H
#define S12_DEBUG_DIAG_H
#if defined(S12_DEBUG_DIAGNOSTICS) && S12_DEBUG_DIAGNOSTICS
#include <stdarg.h>
#include <stdatomic.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define S12DBG_CAP 16384u
#define S12DBG_HEADER 4096u
#ifndef S12DBG_TOKEN_PATH
#define S12DBG_TOKEN_PATH "/s12/private/ota-admin-token"
#endif
#ifndef S12DBG_LOG_PATH
#define S12DBG_LOG_PATH "/run/s12/client.jsonl"
#endif
static struct {
    pthread_mutex_t mutex;pthread_t thread;int socket,started;_Atomic int quit;unsigned char token[64];
    int64_t started_ms,main_ms,reader_ms,dsp_ms;uint64_t generation,source,consumed,direct,beam,idle_blocks;
    unsigned reader_active,reader_error,fifo,phase,muted,gain,paused,afe,selected,locked,fallback,fallback_count;
    double mic_rms[6],mic_noise[6],mic_clip[6];unsigned mic_healthy[6];
    double aec_raw_mean[6],aec_raw[6],aec_clean[6],aec_noise[6],aec_raw_ref,aec_clean_ref;uint64_t aec_generation,aec_raw_end,aec_clean_end;int64_t aec_raw_ms,aec_clean_ms;unsigned aec_gain;
#if defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    struct s12_aec_debug_params aec_params;int64_t aec_params_ms;
#endif
    double cont_last,cont_max;unsigned cont_blocks,cont_streak;
#if defined(S12_DEBUG_MODEL_DIAGNOSTICS) && S12_DEBUG_MODEL_DIAGNOSTICS
    unsigned observe_model_impl_status[2],observe_model_active[2];uint64_t observe_model_previous_begin[2],observe_model_previous_end[2];
    unsigned observe_model_status[2][3],observe_model_frames[2],observe_model_warmup[2],observe_model_flag[2],observe_ref_epoch;double observe_model_score[2],observe_model_threshold[2],observe_model_max[2];uint64_t observe_model_begin[2],observe_model_end[2];int64_t observe_model_ms;
#endif
} s12dbg={.mutex=PTHREAD_MUTEX_INITIALIZER,.socket=-1};
static int s12dbg_add(char *out,size_t cap,size_t *used,const char *format,...) {
    if(*used>=cap)return -1;va_list a;va_start(a,format);int n=vsnprintf(out+*used,cap-*used,format,a);va_end(a);
    if(n<0||(size_t)n>=cap-*used)return -1;*used+=(size_t)n;return 0;
}
static int s12dbg_private_token(const char *path,unsigned char out[64]) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return -1;struct stat st;char raw[66];
    int ok=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&(st.st_mode&0777)==0600&&(st.st_size==64||st.st_size==65);
    ssize_t n=ok?read(fd,raw,sizeof(raw)):-1;close(fd);if(!ok||n!=st.st_size||(n==65&&raw[64]!='\n'))return -1;
    for(unsigned i=0;i<64;++i)if(!((raw[i]>='0'&&raw[i]<='9')||(raw[i]>='a'&&raw[i]<='f')))return -1;
    memcpy(out,raw,64);memset(raw,0,sizeof(raw));return 0;
}
static int s12dbg_equal(const unsigned char *a,const unsigned char *b) {
    volatile unsigned diff=0;for(unsigned i=0;i<64;++i)diff|=(unsigned)(a[i]^b[i]);return !diff;
}
/* Pure header parser: exact fixedroutes, noquery/body/pipeline/duplicateauth. */
static int s12dbg_route(const char *raw,size_t len) {
    if(!len||len>S12DBG_HEADER||memchr(raw,0,len))return 400;
    char request[S12DBG_HEADER+1];memcpy(request,raw,len);request[len]=0;
    char *end=strstr(request,"\r\n\r\n");if(!end||end+4!=request+len)return 400;
    for(size_t i=0;i<len;++i)if((unsigned char)request[i]<32&&request[i]!='\r'&&request[i]!='\n')return 400;
    char *line=strstr(request,"\r\n");if(!line)return 400;*line=0;int route=0;
    if(!strcmp(request,"GET /debug/state HTTP/1.1")||!strcmp(request,"GET /debug/state HTTP/1.0"))route=1;
    else if(!strcmp(request,"GET /debug/log HTTP/1.1")||!strcmp(request,"GET /debug/log HTTP/1.0"))route=2;
    else if(strncmp(request,"GET ",4))route=405;else route=404;
    unsigned auth=0;int valid=0;
    for(char *p=line+2;p<end;) {
        char *next=strstr(p,"\r\n");if(!next)return 400;*next=0;if(!*p)return 400;char *colon=strchr(p,':');if(!colon||colon==p)return 400;
        for(char *k=p;k<colon;++k)if(!isalnum((unsigned char)*k)&&*k!='-')return 400;*colon=0;
        char *value=colon+1;while(*value==' ')++value;
        if(!strcasecmp(p,"Content-Length")||!strcasecmp(p,"Transfer-Encoding")||!strcasecmp(p,"Expect")||!strcasecmp(p,"Upgrade"))return 400;
        if(!strcasecmp(p,"Authorization")) {
            if(++auth!=1)return 400;
            valid=strlen(value)==71&&!memcmp(value,"Bearer ",7)&&s12dbg_equal((unsigned char *)value+7,s12dbg.token);
        }
        p=next+2;
    }
    if(auth!=1||!valid)return 401;return route;
}
static void s12dbg_main(unsigned muted,unsigned gain) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.main_ms=now_ms();s12dbg.muted=muted;s12dbg.gain=gain;pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_phase(const char *name) {
    static const char *const names[]={"starting","ready","busy","paused","muted","recovering","error","closed"};unsigned n=0;
    for(unsigned i=0;i<8;++i)if(!strcmp(name,names[i])){n=i;break;}pthread_mutex_lock(&s12dbg.mutex);s12dbg.phase=n;pthread_mutex_unlock(&s12dbg.mutex);
}
static uint64_t s12dbg_capture_start(void) {
    pthread_mutex_lock(&s12dbg.mutex);uint64_t id=++s12dbg.generation;s12dbg.source=s12dbg.consumed=0;s12dbg.fifo=s12dbg.reader_error=0;s12dbg.reader_active=1;s12dbg.reader_ms=now_ms();pthread_mutex_unlock(&s12dbg.mutex);return id;
}
static void s12dbg_reader(uint64_t id,uint64_t source,unsigned fifo,int error) {
    pthread_mutex_lock(&s12dbg.mutex);if(id==s12dbg.generation){s12dbg.reader_ms=now_ms();s12dbg.source=source;s12dbg.fifo=fifo;s12dbg.reader_error=(unsigned)(error<0?-error:error);}pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_consumed(uint64_t id,uint64_t frames) {
    pthread_mutex_lock(&s12dbg.mutex);if(id==s12dbg.generation)s12dbg.consumed=frames;pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_capture_close(uint64_t id) {
    pthread_mutex_lock(&s12dbg.mutex);if(id==s12dbg.generation)s12dbg.reader_active=0;pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_dsp(uint64_t direct,uint64_t beam,unsigned paused,unsigned afe,const struct mic_frontend *m) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.dsp_ms=now_ms();s12dbg.direct=direct;s12dbg.beam=beam;s12dbg.paused=paused;s12dbg.afe=afe;s12dbg.selected=m->selected;s12dbg.locked=m->locked;
    for(unsigned i=0;i<6;++i){s12dbg.mic_rms[i]=isfinite(m->rms[i])&&m->rms[i]>=0?m->rms[i]:0;s12dbg.mic_noise[i]=isfinite(m->noise[i])&&m->noise[i]>=0?m->noise[i]:0;s12dbg.mic_clip[i]=isfinite(m->clip[i])&&m->clip[i]>=0?m->clip[i]:0;s12dbg.mic_healthy[i]=!!m->healthy[i];}
    ++s12dbg.idle_blocks;pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_fallback(unsigned reason) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.fallback=reason;++s12dbg.fallback_count;pthread_mutex_unlock(&s12dbg.mutex);
}
static size_t s12dbg_state(char out[S12DBG_CAP+1]) {
    pthread_mutex_lock(&s12dbg.mutex);int64_t now=now_ms();
    int n=snprintf(out,S12DBG_CAP+1,"{\"schema\":1,\"debug_only\":true,\"pid\":%ld,\"uptime_ms\":%lld,\"main_heartbeat_age_ms\":%lld,\"reader_heartbeat_age_ms\":%lld,\"dsp_heartbeat_age_ms\":%lld,\"phase\":%u,\"muted\":%u,\"gain\":%u,\"capture_generation\":%llu,\"reader_active\":%u,\"reader_error\":%u,\"source_frames\":%llu,\"consumed_frames\":%llu,\"fifo_frames\":%u,\"direct_frames\":%llu,\"beam_frames\":%llu,\"dsp_blocks\":%llu,\"paused_models\":%u,\"afe_available\":%u,\"selected_channel\":%u,\"frontend_locked\":%u,\"last_timing_fallback\":%u,\"timing_fallback_count\":%u}\n",(long)getpid(),(long long)(now-s12dbg.started_ms),(long long)(s12dbg.main_ms?now-s12dbg.main_ms:-1),(long long)(s12dbg.reader_ms?now-s12dbg.reader_ms:-1),(long long)(s12dbg.dsp_ms?now-s12dbg.dsp_ms:-1),s12dbg.phase,s12dbg.muted,s12dbg.gain,(unsigned long long)s12dbg.generation,s12dbg.reader_active,s12dbg.reader_error,(unsigned long long)s12dbg.source,(unsigned long long)s12dbg.consumed,s12dbg.fifo,(unsigned long long)s12dbg.direct,(unsigned long long)s12dbg.beam,(unsigned long long)s12dbg.idle_blocks,s12dbg.paused,s12dbg.afe,s12dbg.selected,s12dbg.locked,s12dbg.fallback,s12dbg.fallback_count);
    size_t used=n>0&&(unsigned)n<S12DBG_CAP?(size_t)n:0;int bad=!used;
    /* Replace the final objectbrace with fixed sixchannel numericalarrays. */
    if(used>=2){used-=2;out[used]=0;}else bad=1;
    static const char *const keys[3]={"mic_rms","mic_noise","mic_clip"};
    const double *arrays[3]={s12dbg.mic_rms,s12dbg.mic_noise,s12dbg.mic_clip};
    for(unsigned k=0;!bad&&k<3;++k){bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"%s\":[",keys[k]);for(unsigned i=0;!bad&&i<6;++i)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"%s%.4f",i?",":"",arrays[k][i]);if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"]");}
    if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"mic_healthy\":[%u,%u,%u,%u,%u,%u]}\n",s12dbg.mic_healthy[0],s12dbg.mic_healthy[1],s12dbg.mic_healthy[2],s12dbg.mic_healthy[3],s12dbg.mic_healthy[4],s12dbg.mic_healthy[5]);
    if(!bad&&used>=2){used-=2;out[used]=0;bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"aec_generation\":%llu,\"aec_raw_capture_end_frames\":%llu,\"aec_clean_end_10ms_frames\":%llu,\"aec_raw_age_ms\":%lld,\"aec_clean_age_ms\":%lld,\"aec_raw_reference_rms\":%.4f,\"aec_clean_reference_rms\":%.4f,\"aec_reference_gain_permille\":%u",(unsigned long long)s12dbg.aec_generation,(unsigned long long)s12dbg.aec_raw_end,(unsigned long long)s12dbg.aec_clean_end,(long long)(s12dbg.aec_raw_ms?now-s12dbg.aec_raw_ms:-1),(long long)(s12dbg.aec_clean_ms?now-s12dbg.aec_clean_ms:-1),s12dbg.aec_raw_ref,s12dbg.aec_clean_ref,s12dbg.aec_gain);
        const double *aec_arrays[4]={s12dbg.aec_raw,s12dbg.aec_clean,s12dbg.aec_noise,s12dbg.aec_raw_mean};static const char *const aec_keys[4]={"aec_raw_rms","aec_clean_rms","aec_clean_noise","aec_raw_mean_s16"};
        for(unsigned k=0;!bad&&k<4;++k){bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"%s\":[",aec_keys[k]);for(unsigned i=0;!bad&&i<6;++i)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"%s%.4f",i?",":"",aec_arrays[k][i]);if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"]");}if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"}\n");}
#if defined(S12_DEBUG_MODEL_DIAGNOSTICS) && S12_DEBUG_MODEL_DIAGNOSTICS && defined(S12_VENDOR_WAKE_DIAGNOSTICS)
    if(!bad&&used>=2){used-=2;out[used]=0;bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"wake_model_age_ms\":%lld,\"wake_reference_epoch\":%u",(long long)(s12dbg.observe_model_ms?now-s12dbg.observe_model_ms:-1),s12dbg.observe_ref_epoch);
        const char *names[2]={"diversity","reference"};for(unsigned m=0;!bad&&m<2;++m)bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"wake_%s\":{\"status\":[%u,%u,%u],\"l1_score\":%.6f,\"l1_threshold\":%.6f,\"l1_max_score\":%.6f,\"l1_frames\":%u,\"l1_warmup\":%u,\"l1_flag\":%u,\"impl_status\":%u,\"active\":%u,\"previous_begin_frame\":%llu,\"previous_end_frame\":%llu,\"result_begin_frame\":%llu,\"result_end_frame\":%llu}",names[m],s12dbg.observe_model_status[m][0],s12dbg.observe_model_status[m][1],s12dbg.observe_model_status[m][2],s12dbg.observe_model_score[m],s12dbg.observe_model_threshold[m],s12dbg.observe_model_max[m],s12dbg.observe_model_frames[m],s12dbg.observe_model_warmup[m],s12dbg.observe_model_flag[m],s12dbg.observe_model_impl_status[m],s12dbg.observe_model_active[m],(unsigned long long)s12dbg.observe_model_previous_begin[m],(unsigned long long)s12dbg.observe_model_previous_end[m],(unsigned long long)s12dbg.observe_model_begin[m],(unsigned long long)s12dbg.observe_model_end[m]);if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"}\n");}
#endif
#if defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
    if(!bad&&used>=2){unsigned bypass=s12dbg.aec_params.valid_mask==63&&!s12dbg.aec_params.error;for(unsigned c=0;c<6;++c)if(s12dbg.aec_params.channel[c].denoise)bypass=0;used-=2;out[used]=0;bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"aec_params_epoch\":%u,\"aec_params_age_ms\":%lld,\"aec_params_valid_mask\":%u,\"aec_params_error\":%u,\"aec_params_error_channel\":%u,\"aec_all_preprocess_bypassed\":%s,\"aec_params\":[",s12dbg.aec_params.epoch,(long long)(s12dbg.aec_params_ms?now-s12dbg.aec_params_ms:-1),s12dbg.aec_params.valid_mask,s12dbg.aec_params.error,s12dbg.aec_params.error_channel,bypass?"true":"false");for(unsigned c=0;!bad&&c<6;++c){const struct s12_aec_debug_channel_params *d=&s12dbg.aec_params.channel[c];bad=s12dbg_add(out,S12DBG_CAP+1,&used,"%s{\"valid\":%u,\"denoise\":%d,\"noise_suppress\":%d,\"echo_suppress\":%d,\"echo_suppress_active\":%d,\"get_status\":[%d,%d,%d,%d,%d],\"linear_present\":%u,\"preprocess_present\":%u,\"connection_present\":%u,\"connection_matches_linear\":%u}",c?",":"",d->valid,(int)d->denoise,(int)d->noise_suppress,(int)d->echo_suppress,(int)d->echo_suppress_active,(int)d->status[0],(int)d->status[1],(int)d->status[2],(int)d->status[3],(int)d->status[4],d->linear_present,d->preprocess_present,d->connection_present,d->connection_matches_linear);}if(!bad)bad=s12dbg_add(out,S12DBG_CAP+1,&used,"]}\n");}
#endif
    if(!bad&&used>=2){used-=2;out[used]=0;bad=s12dbg_add(out,S12DBG_CAP+1,&used,",\"barge_configured_reference\":%u,\"barge_effective_reference\":%u,\"barge_mic_divisor\":16384,\"barge_reference_divisor\":65536,\"barge_noise_units_multiplier\":4,\"ordinary_mic_divisor\":65536}\n",barge_config.reference,barge_effective_reference());}
    pthread_mutex_unlock(&s12dbg.mutex);return bad?0:used;
}
static int s12dbg_in(const char *s,const char *const *values,size_t n) {for(size_t i=0;i<n;++i)if(!strcmp(s,values[i]))return 1;return 0;}
static int s12dbg_numeric(struct json_object *v) {
    if(json_object_is_type(v,json_type_boolean))return 1;
    if(!json_object_is_type(v,json_type_int)&&!json_object_is_type(v,json_type_double))return 0;
    const char *text=json_object_to_json_string_ext(v,0);if(!text||strlen(text)>48||strspn(text,"0123456789.eE+-")!=strlen(text))return 0;
    char *end=NULL;double x=strtod(text,&end);return end&&!*end&&isfinite(x);
}
/* Safe event/field projection: never return rawlog text, config, token, URLs,
 * ASR/replytext, PCM or unknown/vendorstdout. Each emitted value is allowlisted. */
static size_t s12dbg_filter(const char *line,size_t len,char *out,size_t cap) {
    static const char *const events[]={"local_continuation_started","local_continuation_window","local_continuation_boundary","local_wake_listening","local_bf_noise_ready","local_wake_detected","local_timing_fallback","speech_detected","local_command_frontend","mic_array","capture_reader_started","capture_reader_closed","capture_hardware","local_capture_closed","local_capture_recovering","local_wake_resuming","wake_loop_stopped","utterance_complete","utterance_uploaded","wake_listening","wake_idle","followup_listening","followup_input_prepared","followup_tonal_echo_onset_ignored","quiet_followup_listening","local_command_timeout","local_command_kws_progress","capture_discontinuity","barge_monitor_started","local_barge_detected","barge_capture_handoff","barge_old_turn_discarded","barge_capture_invalidated","ota_idle_enter","ota_idle_exit","panel_turn_cancelled","command_feedback","local_prompt","quiet_followup_accepted"};
    static const char *const numbers[]={"keyword_retained","first_frame","encoded_until","capture_reopened","start","anchor","scan","grace_end","ring_latest","ring_count","hit_channel","onset_first","onset_blocks","probe_blocks","max_streak","last_rms60","max_rms60","outcome","microphones","input_frame","begin_frame","end_frame","noise_rms","raw_quiet_ms","preroll_ms","frame_rms","threshold_rms","onset_block","locked","idle_audio_uploaded","network_required","reader_joined","consumed_frames","source_frames","fifo_peak_frames","queued_frames_discarded","fifo_frames","reader_frames","audio_saved","rate","channels","period_frames","buffer_frames","format_limit_buffer_frames","period_limit_buffer_frames","consecutive_failures","retry_ms","direct_frames_discarded","beam_frames_discarded","old_audio_retried","models_reset","audio_packets","duration_ms","compressed_bytes","padding_samples","max_duration_reached","short_reads_assembled","same_sentence","mic_channel","peak_frame_rms","local_keyword_spotting","window_ms","new_cue","deadline_reset","empty_audio_uploaded","warmup_ms","single_reader","audio_uploaded","direct_frames","beam_frames","ignored_direct_hits","ignored_beam_hits","beam_active","reference_channel","remote_abort_attempted","tail_audio_reused","capture_closed","microphone_uploaded","wake_prompt_replayed","followup_cue_completed","input_already_ready","selected","submitted_rounds","interrupted"};
    static const char *const strings[]={"phase","source","reason","mic_mode","frontend","stage","cause","prompt","capture_owner"};
    static const char *const values[]={"local_wake_idle","selection","utterance_lock","idle","original_beamformer","original_s12_beamformer","six_channel_selection","beam_noise_unavailable","preroll_expired","auto","fixed","read","reader","reader_close","start","xrun","suspended","fifo_overflow","wake","button","command-key","followup","volume","farewell","boot","wifi","cloud","single_reader"};
    if(!len||len>4096||line[0]!='{'||line[len-1]!='}')return 0;
    struct json_tokener *tok=json_tokener_new();if(!tok)return 0;json_tokener_set_flags(tok,1);struct json_object *obj=json_tokener_parse_ex(tok,line,(int)len);int error=json_tokener_get_error(tok);json_tokener_free(tok);
    if(error||!obj||!json_object_is_type(obj,json_type_object)){if(obj)json_object_put(obj);return 0;}
    struct json_object *v=NULL;const char *event=NULL;if(json_object_object_get_ex(obj,"event",&v)&&json_object_is_type(v,json_type_string))event=json_object_get_string(v);
    if(!event||json_object_get_string_len(v)!=(int)strlen(event)||!s12dbg_in(event,events,sizeof(events)/sizeof(events[0]))){json_object_put(obj);return 0;}
    size_t used=0;int failed=s12dbg_add(out,cap,&used,"{\"event\":\"%s\"",event);
    for(size_t i=0;!failed&&i<sizeof(numbers)/sizeof(numbers[0]);++i)if(json_object_object_get_ex(obj,numbers[i],&v)&&s12dbg_numeric(v))failed=s12dbg_add(out,cap,&used,",\"%s\":%s",numbers[i],json_object_to_json_string_ext(v,0));
    for(size_t i=0;!failed&&i<sizeof(strings)/sizeof(strings[0]);++i)if(json_object_object_get_ex(obj,strings[i],&v)&&json_object_is_type(v,json_type_string)) {
        const char *value=json_object_get_string(v);if(value&&json_object_get_string_len(v)==(int)strlen(value)&&s12dbg_in(value,values,sizeof(values)/sizeof(values[0])))failed=s12dbg_add(out,cap,&used,",\"%s\":\"%s\"",strings[i],value);
    }
    if(!failed)failed=s12dbg_add(out,cap,&used,"}\n");json_object_put(obj);return failed?0:used;
}
static size_t s12dbg_log(char out[S12DBG_CAP+1]) {
    int fd=open(S12DBG_LOG_PATH,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return 0;struct stat st;
    int ok=!metadata_stat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&!(st.st_mode&077)&&st.st_size>=0;
    off_t offset=ok&&st.st_size>(off_t)S12DBG_CAP?st.st_size-(off_t)S12DBG_CAP:0;
    char raw[S12DBG_CAP+1];ssize_t n=ok&&lseek(fd,offset,SEEK_SET)>=0?read(fd,raw,S12DBG_CAP):-1;close(fd);if(n<=0)return 0;raw[n]=0;
    size_t used=0,start=0;if(offset){char *first=memchr(raw,'\n',(size_t)n);if(!first)return 0;start=(size_t)(first-raw)+1;}
    while(start<(size_t)n){char *end=memchr(raw+start,'\n',(size_t)n-start);if(!end)break;size_t len=(size_t)(end-(raw+start));char safe[4096];size_t bytes=s12dbg_filter(raw+start,len,safe,sizeof(safe));if(bytes&&bytes<=S12DBG_CAP-used){memcpy(out+used,safe,bytes);used+=bytes;}start+=(len+1);}
    out[used]=0;return used;
}
static int s12dbg_io(int fd,short events,int64_t deadline) {
    while(!stopping&&!atomic_load(&s12dbg.quit)){int ms=remaining(deadline);if(!ms)return -1;if(ms>100)ms=100;struct pollfd p={fd,events,0};int r=poll(&p,1,ms);if(r>0)return p.revents&events?0:-1;if(r<0&&errno!=EINTR)return -1;}return -1;
}
static void s12dbg_serve(int fd) {
    char request[S12DBG_HEADER+1],body[S12DBG_CAP+1],header[512];size_t used=0;int code=400,route=0;int64_t deadline=now_ms()+1000;
    while(used<S12DBG_HEADER&&!stopping){if(s12dbg_io(fd,POLLIN,deadline))break;ssize_t n=recv(fd,request+used,S12DBG_HEADER-used,0);if(n<=0){if(n<0&&(errno==EINTR||errno==EAGAIN))continue;break;}used+=(size_t)n;request[used]=0;if(strstr(request,"\r\n\r\n")){route=s12dbg_route(request,used);break;}}
    size_t length=0;const char *type="application/json";
    if(route==1){length=s12dbg_state(body);code=200;}else if(route==2){length=s12dbg_log(body);code=200;type="application/x-ndjson";}else {code=route>=400?route:400;length=(size_t)snprintf(body,sizeof(body),"{\"error\":\"request_rejected\"}\n");}
    const char *reason=code==200?"OK":code==401?"Unauthorized":code==404?"Not Found":code==405?"Method Not Allowed":"Bad Request";
    int n=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nPragma: no-cache\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",code,reason,type,length);
    if(n<=0||(size_t)n>=sizeof(header))return;const char *parts[2]={header,body};size_t sizes[2]={(size_t)n,length};
    for(unsigned p=0;p<2;++p)for(size_t at=0;at<sizes[p]&&!stopping;){if(s12dbg_io(fd,POLLOUT,deadline))return;ssize_t sent=send(fd,parts[p]+at,sizes[p]-at,MSG_NOSIGNAL);if(sent>0)at+=(size_t)sent;else if(sent<0&&(errno==EINTR||errno==EAGAIN))continue;else return;}
    memset(request,0,sizeof(request));
}
static void *s12dbg_thread(void *unused) {
    (void)unused;while(!stopping&&!atomic_load(&s12dbg.quit)){struct pollfd p={s12dbg.socket,POLLIN,0};int r=poll(&p,1,100);if(r<=0)continue;struct sockaddr_in peer;socklen_t bytes=sizeof(peer);int fd=accept(s12dbg.socket,(struct sockaddr *)&peer,&bytes);if(fd<0)continue;
        (void)fcntl(fd,F_SETFD,FD_CLOEXEC);int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)){close(fd);continue;}
        uint32_t ip=ntohl(peer.sin_addr.s_addr);if((ip>>24)==10||(ip>>20)==0xac1||(ip>>16)==0xc0a8||(ip>>24)==127)s12dbg_serve(fd);close(fd);
    }return NULL;
}

/* Main/DSPowners publish scalarenergy only; noPCM escapes thesecalls. Rawlast
 * hardwarechunk andaligned cleanpacket have separateframeaxes andtimestamps. */
static void s12dbg_aec_raw(uint64_t generation,uint64_t frames,const int16_t raw[6][UPLINK_SAMPLES],const int16_t *reference) {
    /* Raw here is the original pre-AEC FIR-decimated S16 input. FIR taps
     * sum to1 after the existing S32/65536 conversion, so its block mean
     * measures ADC bias in the same units without altering any sample. */
    double rms[6],mean[6];for(unsigned i=0;i<6;++i){int64_t sum=0;for(unsigned n=0;n<UPLINK_SAMPLES;++n)sum+=raw[i][n];mean[i]=(double)sum/UPLINK_SAMPLES;rms[i]=mic_rms(raw[i],UPLINK_SAMPLES);}double ref=mic_rms(reference,UPLINK_SAMPLES);
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.aec_generation=generation;s12dbg.aec_raw_end=frames;s12dbg.aec_raw_ms=now_ms();s12dbg.aec_raw_ref=ref;memcpy(s12dbg.aec_raw,rms,sizeof(rms));memcpy(s12dbg.aec_raw_mean,mean,sizeof(mean));pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_aec_clean(uint64_t generation,uint64_t frames,const int16_t clean[6][UPLINK_SAMPLES],const int16_t *reference,const struct mic_frontend *m,unsigned gain) {
    double rms[6];for(unsigned i=0;i<6;++i)rms[i]=mic_rms(clean[i],UPLINK_SAMPLES);double ref=mic_rms(reference,UPLINK_SAMPLES);
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.aec_generation=generation;s12dbg.aec_clean_end=frames;s12dbg.aec_clean_ms=now_ms();s12dbg.aec_clean_ref=ref;s12dbg.aec_gain=gain;memcpy(s12dbg.aec_clean,rms,sizeof(rms));for(unsigned i=0;i<6;++i)s12dbg.aec_noise[i]=m->noise[i];pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_cont_window(const char *source,unsigned ch,uint64_t begin,uint64_t end,uint64_t emit,uint64_t start,uint64_t anchor,uint64_t scan,uint64_t grace,uint64_t latest,unsigned count,double threshold,double noise) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.cont_last=s12dbg.cont_max=0;s12dbg.cont_blocks=s12dbg.cont_streak=0;pthread_mutex_unlock(&s12dbg.mutex);
    printf("{\"event\":\"local_continuation_window\",\"source\":\"%s\",\"hit_channel\":%u,\"begin_frame\":%llu,\"end_frame\":%llu,\"input_frame\":%llu,\"start\":%llu,\"anchor\":%llu,\"scan\":%llu,\"grace_end\":%llu,\"ring_latest\":%llu,\"ring_count\":%u,\"threshold_rms\":%.3f,\"noise_rms\":%.3f}\n",source,ch,(unsigned long long)begin,(unsigned long long)end,(unsigned long long)emit,(unsigned long long)start,(unsigned long long)anchor,(unsigned long long)scan,(unsigned long long)grace,(unsigned long long)latest,count,threshold,noise);
}
static void s12dbg_cont_probe(double rms,unsigned streak) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.cont_last=rms;if(rms>s12dbg.cont_max)s12dbg.cont_max=rms;++s12dbg.cont_blocks;if(streak>s12dbg.cont_streak)s12dbg.cont_streak=streak;pthread_mutex_unlock(&s12dbg.mutex);
}
static void s12dbg_cont_end(unsigned outcome,uint64_t start,uint64_t scan,uint64_t grace,uint64_t latest,uint64_t first,unsigned blocks,double threshold) {
    pthread_mutex_lock(&s12dbg.mutex);double last=s12dbg.cont_last,max=s12dbg.cont_max;unsigned n=s12dbg.cont_blocks,streak=s12dbg.cont_streak;pthread_mutex_unlock(&s12dbg.mutex);
    printf("{\"event\":\"local_continuation_boundary\",\"outcome\":%u,\"start\":%llu,\"scan\":%llu,\"grace_end\":%llu,\"ring_latest\":%llu,\"onset_first\":%llu,\"onset_blocks\":%u,\"probe_blocks\":%u,\"max_streak\":%u,\"last_rms60\":%.3f,\"max_rms60\":%.3f,\"threshold_rms\":%.3f}\n",outcome,(unsigned long long)start,(unsigned long long)scan,(unsigned long long)grace,(unsigned long long)latest,(unsigned long long)first,blocks,n,streak,last,max,threshold);
}

#if defined(S12_DEBUG_MODEL_DIAGNOSTICS) && S12_DEBUG_MODEL_DIAGNOSTICS && defined(S12_VENDOR_WAKE_DIAGNOSTICS)
/* Existing adapter getter only; no additional OEM-memory offsets. Called on
 * the sole DSP owner after the reference and diversity models have advanced. */
static void s12dbg_wake_models(s12_vendor_wake *near,s12_vendor_wake *reference,unsigned epoch,int is_beam) {
    if(!near||!reference)return;struct s12_vendor_wake_diagnostics d[2];memset(d,0,sizeof(d));s12_vendor_wake_diagnostics(near,&d[0]);s12_vendor_wake_diagnostics(reference,&d[1]);
    pthread_mutex_lock(&s12dbg.mutex);for(unsigned i=0;i<2;++i){memcpy(s12dbg.observe_model_status[i],d[i].status,sizeof(d[i].status));s12dbg.observe_model_frames[i]=d[i].l1_frames;s12dbg.observe_model_warmup[i]=d[i].l1_warmup;s12dbg.observe_model_flag[i]=d[i].l1_flag;s12dbg.observe_model_impl_status[i]=d[i].impl_status;s12dbg.observe_model_active[i]=d[i].active;s12dbg.observe_model_previous_begin[i]=d[i].previous_begin_frame;s12dbg.observe_model_previous_end[i]=d[i].previous_end_frame;s12dbg.observe_model_score[i]=isfinite(d[i].l1_score)?d[i].l1_score:0;s12dbg.observe_model_threshold[i]=isfinite(d[i].l1_threshold)?d[i].l1_threshold:0;s12dbg.observe_model_max[i]=isfinite(d[i].l1_max_score)?d[i].l1_max_score:0;s12dbg.observe_model_begin[i]=d[i].result_begin_frame;s12dbg.observe_model_end[i]=d[i].result_end_frame;}(void)is_beam;s12dbg.observe_ref_epoch=epoch;s12dbg.observe_model_ms=now_ms();pthread_mutex_unlock(&s12dbg.mutex);
}
#else
#define s12dbg_wake_models(...) ((void)0)
#endif
#if defined(S12_DEBUG_AEC_PARAMS) && S12_DEBUG_AEC_PARAMS
void s12_debug_aec_params(const struct s12_aec_debug_params *params) {
    pthread_mutex_lock(&s12dbg.mutex);s12dbg.aec_params=*params;s12dbg.aec_params_ms=now_ms();pthread_mutex_unlock(&s12dbg.mutex);
}
#endif
static int s12dbg_start(void) {
    if(s12dbg_private_token(S12DBG_TOKEN_PATH,s12dbg.token))return -1;int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    (void)fcntl(fd,F_SETFD,FD_CLOEXEC);int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)){close(fd);memset(s12dbg.token,0,64);return -1;}int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));struct sockaddr_in addr;memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_port=htons(8034);addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))||listen(fd,2)){close(fd);memset(s12dbg.token,0,64);return -1;}
    s12dbg.socket=fd;s12dbg.started_ms=now_ms();if(pthread_create(&s12dbg.thread,NULL,s12dbg_thread,NULL)){close(fd);s12dbg.socket=-1;return -1;}s12dbg.started=1;return 0;
}
static void s12dbg_stop(void) {
    if(!s12dbg.started)return;/* Calledafter mainloopstopped; no DSP/capturelocks held. */
    atomic_store(&s12dbg.quit,1);(void)pthread_join(s12dbg.thread,NULL);close(s12dbg.socket);s12dbg.socket=-1;s12dbg.started=0;memset(s12dbg.token,0,64);
}
#else
#define s12dbg_wake_models(...) ((void)0)
#define s12dbg_aec_raw(...) ((void)0)
#define s12dbg_aec_clean(...) ((void)0)
#define s12dbg_cont_window(...) ((void)0)
#define s12dbg_cont_probe(...) ((void)0)
#define s12dbg_cont_end(...) ((void)0)
#define s12dbg_main(m,g) ((void)0)
#define s12dbg_phase(s) ((void)0)
#define s12dbg_capture_start() 0u
#define s12dbg_reader(i,s,f,e) ((void)0)
#define s12dbg_consumed(i,f) ((void)0)
#define s12dbg_capture_close(i) ((void)0)
#define s12dbg_dsp(d,b,p,a,m) ((void)0)
#define s12dbg_fallback(r) ((void)0)
#define s12dbg_start() 0
#define s12dbg_stop() ((void)0)
#endif
#endif
