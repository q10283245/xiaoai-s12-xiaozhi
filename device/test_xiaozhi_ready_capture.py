#!/usr/bin/env python3
"""Ready-before-cue capture: synthetic8ch PCM/Opus, no hardware or cloud."""
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import json,math,os,struct,subprocess,tempfile,unittest
HERE=Path(__file__).resolve().parent
HARNESS=r'''
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define XIAOZHI_TEST_NO_ALSA
#define XIAOZHI_TEST_FAKE_CAPTURE
#include <stdint.h>
static double ready_fixture_sample(double t,unsigned channel);
static void ready_raw_check(const int32_t *raw,unsigned frames,uint64_t origin,uint64_t offset);
#define S12_TEST_READY_RAW_CHECK(raw,n,origin,offset) ready_raw_check(raw,n,origin,offset)
#define S12_TEST_READY_PCM_SAMPLE(t,ch) ready_fixture_sample(t,ch)
#define main client_program_main
#include "vendor_wake_test.c"
#include "xiaozhi_client.c"
#undef main
#include <assert.h>
static int16_t fixture_pcm[32000];static unsigned fixture_samples;
static double ready_fixture_sample(double t,unsigned channel) {
    if(!fixture_samples)return NAN;unsigned at=(unsigned)(t*16000);
    if(at>=fixture_samples)return 0;return fixture_pcm[at]*(channel==2?.1:channel==4?-.1:channel==3?.002:0);
}
static unsigned first_raw_frames;
static void ready_raw_check(const int32_t *raw,unsigned frames,uint64_t origin,uint64_t offset) {
    if(!fixture_samples||offset)return;
    for(unsigned i=0;i<frames;++i) {
        double t=(double)i/48000,phase=(double)(origin+i)/48000;
        int32_t expected=(int32_t)(65536.0*(4000+8*sin(2*PI*273*phase)+ready_fixture_sample(t,2)));
        assert(llabs((long long)raw[i*CAPTURE_CHANNELS+2]-expected)<=1);
    }first_raw_frames=frames;
}
static double prefix_correlation(const int16_t *decoded,unsigned samples) {
    /* Compare the head against the known production FIR output, rather than
     * expecting lossy Opus to preserve6..8kHz frication rejected by that FIR. */
    int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];int16_t reference[33000];unsigned used=0;struct decimator filter;decimator_init(&filter);
    for(unsigned base=0;base<fixture_samples;base+=UPLINK_SAMPLES) {
        memset(raw,0,sizeof(raw));for(unsigned i=0;i<CAPTURE_CHUNK;++i){unsigned at=base+i/3;if(at<fixture_samples)raw[i*CAPTURE_CHANNELS+2]=(int32_t)(fixture_pcm[at]*.1*65536);}
        used+=decimate(&filter,raw,CAPTURE_CHUNK,2,reference+used);
    }
    unsigned first=0;while(first<fixture_samples&&abs(fixture_pcm[first])<30)++first;
    unsigned length=1600;if(first+length>fixture_samples)length=fixture_samples-first;assert(length>=640);
    double best=0;for(unsigned delay=0;delay<800&&first+length+delay<samples;++delay) {
        double mx=0,my=0;for(unsigned i=0;i<length;++i){mx+=reference[first+i];my+=decoded[first+i+delay];}mx/=length;my/=length;
        double xy=0,xx=0,yy=0;for(unsigned i=0;i<length;++i){double x=reference[first+i]-mx,y=decoded[first+i+delay]-my;xy+=x*y;xx+=x*x;yy+=y*y;}
        double r=xy/sqrt(xx*yy+1);if(r>best)best=r;
    }return best;
}
static double bin_energy(const int16_t *pcm,unsigned start,unsigned end,double hz) {
    double re=0,im=0;for(unsigned i=start;i<end;++i){double phase=2*PI*hz*i/16000;re+=pcm[i]*cos(phase);im+=pcm[i]*sin(phase);}return re*re+im*im;
}
struct ready_control_change {const char *directory,*action,*phase;};
static void control_file(const char *dir,const char *text) {
    char path[4096],next[4096];snprintf(path,sizeof(path),"%s/panel.control",dir);snprintf(next,sizeof(next),"%s/next",dir);
    int fd=open(next,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));close(fd);assert(!rename(next,path));
}
static void ready_control_apply(const char *dir,const char *action) {
    if(!strcmp(action,"term")){stopping=1;return;}
    if(!strcmp(action,"ota")) {
        char path[4096];snprintf(path,sizeof(path),"%s/auto-apply.request",dir);int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
        char nonce[49];memset(nonce,'a',48);nonce[48]='\n';assert(write(fd,nonce,sizeof(nonce))==sizeof(nonce));close(fd);return;
    }
    control_file(dir,!strcmp(action,"mute")?"v1 1 50 1 0 0\n":!strcmp(action,"manual")?"v1 1 50 0 0 1\n":"v1 1 50 0 1 0\n");
}
static void *ready_control_thread(void *opaque) {
    struct ready_control_change *c=opaque;
    if(!strcmp(c->phase,"before")) {
        char idle[4096],request[4096];snprintf(idle,sizeof(idle),"%s/auto-apply.idle",c->directory);snprintf(request,sizeof(request),"%s/auto-apply.request",c->directory);
        int64_t end=now_ms()+1200;while(access(idle,F_OK)&&remaining(end)){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}assert(!access(idle,F_OK));assert(!unlink(request));return NULL;
    }
    if(!strcmp(c->phase,"after")) {
        char state[4096],value[32];snprintf(state,sizeof(state),"%s/ui.state",c->directory);int ready=0;int64_t end=now_ms()+1500;
        while(!ready&&remaining(end)) {int fd=open(state,O_RDONLY);if(fd>=0){ssize_t n=read(fd,value,sizeof(value));close(fd);ready=n==10&&(!memcmp(value,"listening\n",10)||!memcmp(value,"capturing\n",10));}if(!ready){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}}assert(ready);
    } else {struct timespec pause={0,!strcmp(c->phase,"warm")?50000000:230000000};nanosleep(&pause,NULL);}
    ready_control_apply(c->directory,c->action);
    if(!strcmp(c->action,"ota")&&!strcmp(c->phase,"after")) {
        char idle[4096],request[4096];snprintf(idle,sizeof(idle),"%s/auto-apply.idle",c->directory);snprintf(request,sizeof(request),"%s/auto-apply.request",c->directory);
        int64_t end=now_ms()+1200;while(access(idle,F_OK)&&remaining(end)){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}assert(!access(idle,F_OK));assert(!unlink(request));
    }
    return NULL;
}
static int ready_control_case(const char *dir,const char *action,const char *phase) {
    local_wake_enabled=1;followup_seconds=8;followup_prompt_pending=1;followup_capture_deadline=now_ms()+10000;
    panel_enabled=1;panel_active=1;setenv("S12_TEST_PANEL_DIR",dir,1);setenv("S12_TEST_OTA_DIR",dir,1);
    setenv("S12_TEST_CAPTURE_PROFILE","wake-silence",1);setenv("S12_TEST_CAPTURE_FAST","0",1);setenv("S12_TEST_READY_SHORT_MS","0",1);setenv("S12_TEST_UI_PROMPT_MS","300",1);
    control_file(dir,"v1 0 50 0 0 0\n");panel_poll();
    struct ready_control_change change={dir,action,phase};pthread_t worker;int threaded=strcmp(phase,"before")||!strcmp(action,"ota");
    if(!strcmp(phase,"before"))ready_control_apply(dir,action);
    if(threaded)assert(!pthread_create(&worker,NULL,ready_control_thread,&change));
    struct local_chain chain={0};assert(!local_chain_models(&chain));struct mic_frontend frontend;mic_frontend_init(&frontend,MIC_AUTO);struct vad v;vad_init(&v,30);
    struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(WAKE_MAX_PACKETS,sizeof(*q.slices));struct audio options={0};options.gain=50;
    int rc=capture_utterance(&v,&frontend,&q,&chain,5000,&options);if(threaded)assert(!pthread_join(worker,NULL));
    assert(rc==2||(!strcmp(action,"term")&&rc==1)||(rc<0&&panel_interrupted));assert(!q.count&&!q.used&&!command_feedback_active&&!followup_prompt_pending);
    free(q.bytes);free(q.slices);local_chain_drop(&chain);puts("{\"verified\":true,\"control_preempted\":true,\"audio_uploaded\":false}");return 0;
}
static int ready_budget_case(const char *phase) {
    local_wake_enabled=1;followup_seconds=8;followup_prompt_pending=1;
    followup_capture_deadline=now_ms()+(!strcmp(phase,"warm")?50:300);
    setenv("S12_TEST_CAPTURE_PROFILE","wake-silence",1);setenv("S12_TEST_CAPTURE_FAST","0",1);setenv("S12_TEST_READY_SHORT_MS","0",1);setenv("S12_TEST_UI_PROMPT_MS","300",1);
    struct local_chain chain={0};assert(!local_chain_models(&chain));struct mic_frontend frontend;mic_frontend_init(&frontend,MIC_AUTO);struct vad v;vad_init(&v,30);
    struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(WAKE_MAX_PACKETS,sizeof(*q.slices));struct audio options={0};options.gain=50;
    int rc=capture_utterance(&v,&frontend,&q,&chain,20000,&options);assert(rc==3&&!q.count&&!q.used&&!followup_prompt_pending&&!command_feedback_active);
    free(q.bytes);free(q.slices);local_chain_drop(&chain);puts("{\"verified\":true,\"total_budget_expired\":true,\"audio_uploaded\":false}");return 0;
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"budget"))return ready_budget_case(argv[2]);
    if(argc==5&&!strcmp(argv[1],"control"))return ready_control_case(argv[2],argv[3],argv[4]);
    const char *fixture_path=getenv("S12_TEST_READY_REAL_PCM");
    if(fixture_path){int fd=open(fixture_path,O_RDONLY);assert(fd>=0);ssize_t bytes=read(fd,fixture_pcm,sizeof(fixture_pcm));assert(bytes>0&&bytes%2==0&&(size_t)bytes<sizeof(fixture_pcm));close(fd);fixture_samples=(unsigned)bytes/2;}
    assert(argc==3);unsigned duration=(unsigned)strtoul(argv[1],NULL,10),onset=(unsigned)strtoul(argv[2],NULL,10);
    local_wake_enabled=1;followup_seconds=8;followup_prompt_pending=1;followup_capture_deadline=now_ms()+20000;
    struct local_chain chain={0};assert(!local_chain_models(&chain));
    if(getenv("S12_TEST_READY_BEAM_PRIOR")){chain.beam_noise=8;chain.beam_noise_ready=1;}
    struct mic_frontend frontend;mic_frontend_init(&frontend,MIC_AUTO);struct vad v;vad_init(&v,30);
    struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(TTS_BUFFER_PACKETS,sizeof(*q.slices));assert(q.bytes&&q.slices);
    struct audio options={0};options.gain=50;int64_t began=now_ms();unsigned wait=duration?2000:600;
    int rc=capture_utterance(&v,&frontend,&q,&chain,wait,&options);
    assert(!command_feedback_active&&!followup_prompt_pending);
    if(duration) {
        assert(!rc&&q.count>0);int err=0;OpusDecoder *decoder=opus_decoder_create(16000,1,&err);assert(decoder&&!err);
        int16_t decoded[WAKE_MAX_PACKETS*UPLINK_SAMPLES];unsigned samples=0;
        for(unsigned i=0;i<q.count;++i){int n=opus_decode(decoder,q.bytes+q.slices[i].offset,(int)q.slices[i].len,decoded+samples,UPLINK_SAMPLES,0);assert(n==UPLINK_SAMPLES);samples+=(unsigned)n;}
        opus_decoder_destroy(decoder);
        unsigned begin=onset*16+160,end=onset*16+960;assert(end<=samples);
        const char *user_hz=getenv("S12_TEST_READY_USER_HZ");double prefix=bin_energy(decoded,begin,end,user_hz?strtod(user_hz,NULL):443),body=bin_energy(decoded,begin,end,997);
        double correlation=fixture_samples?prefix_correlation(decoded,samples):0;
        if(fixture_samples){assert(first_raw_frames==CAPTURE_CHUNK);if(getenv("S12_TEST_READY_CORRELATION_REQUIRED"))assert(correlation>.65);}else assert(prefix>body*4&&prefix>100000000);
        printf("{\"verified\":true,\"duration_ms\":%u,\"onset_ms\":%u,\"packets\":%u,\"prefix_ratio\":%.2f,\"real_prefix_correlation\":%.4f,\"first_raw_frames_exact\":%u}\n",duration,onset,q.count,prefix/(body+1),correlation,first_raw_frames);
        wipe(decoded,sizeof(decoded));
    } else {
        assert(rc==3&&!q.count&&!q.used);assert(followup_onset_deadline>=began+wait);
        printf("{\"verified\":true,\"cue_or_echo_only_uploaded\":false,\"window_started_after_cue\":true}\n");
    }
    wipe(q.bytes,TTS_BUFFER_BYTES);free(q.bytes);free(q.slices);local_chain_drop(&chain);return 0;
}
'''
class ReadyCaptureTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='s12-ready-capture-');cls.root=Path(cls.temp.name);source=cls.root/'ready.c';source.write_text(HARNESS);cls.binary=cls.root/'ready'
  subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(HERE),str(source),*native_lib_flags(),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def run_capture(self,duration,onset=0,**extra):
  env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',S12_TEST_CAPTURE_PROFILE='wake-array',S12_TEST_CAPTURE_FAST='1',S12_TEST_READY_SHORT_MS=str(duration),S12_TEST_READY_ONSET_MS=str(onset),S12_TEST_UI_PROMPT_MS='20');env.update(extra)
  r=subprocess.run([str(self.binary),str(duration),str(onset)],env=env,capture_output=True,timeout=6)
  self.assertEqual(r.returncode,0,r.stdout+r.stderr)
  events=[json.loads(x) for x in r.stdout.splitlines()]
  self.assertEqual(sum(e.get('event')=='capture_reader_started' for e in events),1)
  self.assertEqual(sum(e.get('event')=='capture_reader_closed' for e in events),1)
  cue=next(i for i,e in enumerate(events) if e.get('event')=='local_prompt' and e['prompt']=='followup')
  warm=next(i for i,e in enumerate(events) if e.get('event')=='followup_input_prepared');ready=next(i for i,e in enumerate(events) if e.get('event')=='followup_listening')
  self.assertLess(warm,cue);self.assertLess(cue,ready)
  if extra.get('S12_TEST_READY_BEAM_PRIOR'):
   self.assertTrue(any(e.get('event')=='local_command_frontend' and e.get('source')=='original_beamformer' for e in events))
  if extra.get('S12_TEST_READY_FORCE_BOUNDARY'):
   boundary=next(e for e in events if e.get('event')=='followup_eof_boundary_fixture')
   self.assertTrue(boundary['inflight_read_forced']);self.assertGreater(boundary['retained_frames'],0);self.assertLessEqual(boundary['retained_frames'],48);self.assertEqual(boundary['first_source_frame'],0)
  return events[-1]
 def test_total_budget_expiry_during_prepare_never_announces_ready(self):
  for phase in ['warm','cue']:
   with self.subTest(phase=phase):
    r=subprocess.run([str(self.binary),'budget',phase],capture_output=True,timeout=4,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
    self.assertEqual(r.returncode,0,r.stdout+r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
    self.assertFalse(any(e.get('event') in ['followup_listening','utterance_complete'] for e in events))
    self.assertEqual(sum(e.get('event')=='capture_reader_started' for e in events),1)
    self.assertEqual(sum(e.get('event')=='capture_reader_closed' for e in events),1)
 def test_controls_before_warm_during_cue_and_immediately_after_ready(self):
  for phase in ['before','warm','cue','after']:
   for action in ['mute','cancel','manual','ota','term']:
    with self.subTest(phase=phase,action=action),tempfile.TemporaryDirectory(dir=self.root) as td:
     r=subprocess.run([str(self.binary),'control',td,action,phase],capture_output=True,timeout=5,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
     self.assertEqual(r.returncode,0,r.stdout+r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
     started=sum(e.get('event')=='capture_reader_started' for e in events);closed=sum(e.get('event')=='capture_reader_closed' for e in events)
     self.assertEqual(started,0 if phase=='before' else 1);self.assertEqual(started,closed)
     self.assertFalse(any(e.get('event')=='utterance_complete' for e in events))
 def test_short_utterance_prefix_survives_immediate_ready_boundary(self):
  for duration in [180,240,300,400,600]:
   for onset in [0,20,60]:
    with self.subTest(duration=duration,onset=onset):self.run_capture(duration,onset)
 def generated_pcm(self,duration):
  # Unique deterministic harmonic/chirp prefix; no human recording is used.
  samples=[round(3000*(math.sin(2*math.pi*(211*i/16000+137*(i/16000)**2))+.35*math.sin(2*math.pi*613*i/16000))) for i in range(duration*16)]
  path=self.root/('generated-'+str(duration)+'.pcm');path.write_bytes(struct.pack('<'+'h'*len(samples),*samples));return path
 def test_generated_short_prefix_survives_ready_boundary(self):
  for duration in [265,475]:
   with self.subTest(milliseconds=duration):
    path=self.generated_pcm(duration)
    result=self.run_capture(duration,S12_TEST_READY_REAL_PCM=str(path),S12_TEST_READY_FORCE_BOUNDARY='1',S12_TEST_UI_PROMPT_MS='20',S12_TEST_READY_CORRELATION_REQUIRED='1')
    self.assertEqual(result['first_raw_frames_exact'],2880);self.assertGreater(result['real_prefix_correlation'],.65)
 def test_generated_prefix_with_available_afe_and_one_two_frame_delay(self):
  path=self.generated_pcm(475)
  for delay in [1,2]:
   with self.subTest(afe_delay_frames=delay):
    result=self.run_capture(475,S12_TEST_READY_REAL_PCM=str(path),S12_TEST_READY_FORCE_BOUNDARY='1',S12_TEST_READY_CORRELATION_REQUIRED='1',S12_TEST_AFE='pass',S12_TEST_AFE_DELAY_FRAMES=str(delay),S12_TEST_READY_BEAM_PRIOR='1')
    self.assertEqual(result['first_raw_frames_exact'],2880);self.assertGreater(result['real_prefix_correlation'],.65)
 def test_cue_eof_inflight_block_is_kept_before_reader_ack(self):
  for duration in [180,300]:
   with self.subTest(duration=duration):
    result=self.run_capture(duration,S12_TEST_READY_FORCE_BOUNDARY='1',S12_TEST_UI_PROMPT_MS='20')
    self.assertTrue(result['verified'])
 def test_short_prefix_with_cue_decay_keeps_user_pcm(self):
  for duration in [180,300,600]:
   with self.subTest(duration=duration):self.run_capture(duration,S12_TEST_READY_ECHO_MS='300',S12_TEST_READY_ECHO_RMS='700')
 def test_harmonic_speech_and_cue_frequency_prefix_are_preserved(self):
  for duration in [180,300,600]:
   with self.subTest(duration=duration):
    self.run_capture(duration,S12_TEST_READY_USER_HZ='550',S12_TEST_READY_USER_HARMONIC='0.3',S12_TEST_READY_USER_DECAY_MS='900')
    self.run_capture(duration,S12_TEST_READY_USER_HZ='550')
  self.run_capture(180,180,S12_TEST_READY_ECHO_MS='300',S12_TEST_READY_ECHO_RMS='700')
 def test_cue_and_reasonable_decaying_echo_never_upload(self):
  self.run_capture(0)
  for rms in [50,200,700]:
   with self.subTest(rms=rms):self.run_capture(0,S12_TEST_READY_ECHO_MS='300',S12_TEST_READY_ECHO_RMS=str(rms))
if __name__=='__main__':unittest.main()
