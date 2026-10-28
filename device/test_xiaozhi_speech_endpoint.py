#!/usr/bin/env python3
"""Production endpoint logic with generated harmonic PCM; no recordings or ASR quality claim."""
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import json,math,os,random,struct,subprocess,tempfile,unittest
import test_xiaozhi_ready_capture as ready
import test_xiaozhi_client as base
import test_xiaozhi_followup as follow
import test_xiaozhi_dialogue as dialogue
HERE=Path(__file__).resolve().parent
HARNESS=ready.HARNESS.replace('fixture_pcm[32000]','fixture_pcm[240000]')
HARNESS=HARNESS.replace('base<fixture_samples;base+=UPLINK_SAMPLES','base<fixture_samples&&base<3200;base+=UPLINK_SAMPLES')
HARNESS=HARNESS.replace('static unsigned first_raw_frames;','static unsigned first_raw_frames;static uint64_t observed_input_frames;')
HARNESS=HARNESS.replace('''    if(!fixture_samples||offset)return;''','''    if(offset+frames>observed_input_frames)observed_input_frames=offset+frames;
    if(!fixture_samples||offset)return;''')
HARNESS=HARNESS.replace('''        double correlation=fixture_samples?prefix_correlation(decoded,samples):0;''','''        const char *required=getenv("S12_TEST_EXPECT_INPUT_FRAMES");if(required){assert(observed_input_frames>=strtoull(required,0,10));}
        printf("{\\"event\\":\\"endpoint_test_stats\\",\\"packets\\":%u,\\"speech_frames\\":%u,\\"vad_frames\\":%u,\\"max_silence_ms\\":%u,\\"max_reason\\":%u,\\"observed_input_frames\\":%llu}\\n",q.count,q.capture_stats.speech_frames,q.capture_stats.vad_frames,q.capture_stats.max_silence_ms,q.capture_stats.max_reason,(unsigned long long)observed_input_frames);
        assert(q.capture_stats.valid&&q.capture_stats.speech_frames<=q.capture_stats.vad_frames&&q.capture_stats.vad_frames<=q.count*6);
        double correlation=fixture_samples?prefix_correlation(decoded,samples):0;''')
HARNESS=HARNESS.replace('''    local_wake_enabled=1;followup_seconds=8;followup_prompt_pending=1;followup_capture_deadline=now_ms()+20000;''','''    local_wake_enabled=1;followup_seconds=8;followup_prompt_pending=1;followup_capture_deadline=now_ms()+20000;''')
# This direct test checks a trained instance survives relisten, rather than
# substituting a fake speech classifier for the actual implementation.
HARNESS=HARNESS.replace('''int main(int argc,char **argv) {''','''static int speech_lifetime(void) {
    struct vad v;vad_init(&v,30);int16_t pcm[UPLINK_SAMPLES];for(unsigned i=0;i<UPLINK_SAMPLES;++i)pcm[i]=(int16_t)(35*1.41421356237*sin(2*PI*311*i/16000));
    for(unsigned i=0;i<12;++i)assert(command_speech_frames(&v,pcm)>=0);Fvad *same=v.speech;assert(same);vad_relisten(&v);
    for(unsigned i=0;i<3;++i)assert(command_speech_frames(&v,pcm)==0&&v.speech==same);fvad_free(v.speech);v.speech=NULL;return 0;
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"lifetime"))return speech_lifetime();''')
HARNESS=HARNESS.replace('local_chain_drop(&chain);','local_chain_drop(&chain);if(v.speech){fvad_free(v.speech);v.speech=NULL;}')
HARNESS=HARNESS.replace('        assert(!rc&&q.count>0);','        if(rc==3&&getenv("S12_TEST_EXPECT_ONSET_TIMEOUT")){assert(!q.count&&!q.used);free(q.bytes);free(q.slices);local_chain_drop(&chain);if(v.speech)fvad_free(v.speech);puts("{\\"event\\":\\"expected_legacy_onset_timeout\\"}");return 0;}\n        assert(!rc&&q.count>0);')

def synthetic_voice(ms=600):
    # Deterministic harmonic bursts, generated mathematically in RAM. These
    # exercise PCM retention/endpoint fallback, not natural speech recognition.
    count=ms*16
    return [(0.7+0.3*math.cos(2*math.pi*2.5*i/16000))*
            (math.sin(2*math.pi*211*i/16000)+.45*math.sin(2*math.pi*422*i/16000)+.2*math.sin(2*math.pi*633*i/16000))
            for i in range(count)]
def normalize(samples,rms):
    mean=sum(samples)/len(samples);current=math.sqrt(sum((x-mean)**2 for x in samples)/len(samples));return [(x-mean)*rms/current for x in samples]
def save_pcm(path,samples):path.write_bytes(struct.pack('<'+'h'*len(samples),*[max(-32768,min(32767,round(x))) for x in samples]))

class SpeechEndpointTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='s12-speech-endpoint-');cls.root=Path(cls.temp.name);src=cls.root/'speech.c';src.write_text(HARNESS);cls.binary=cls.root/'speech'
        subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(HERE),str(src),*native_lib_flags(),'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def capture(self,samples,voice_end=None,expect_timeout=False,**extra):
        fixture=self.root/'input.pcm';save_pcm(fixture,samples);voice_end=voice_end if voice_end is not None else len(samples)
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1',S12_TEST_CAPTURE_PROFILE='wake-array',S12_TEST_CAPTURE_FAST='1',S12_TEST_READY_SHORT_MS='12000',S12_TEST_READY_ONSET_MS='0',S12_TEST_UI_PROMPT_MS='20',S12_TEST_READY_REAL_PCM=str(fixture),S12_TEST_READY_FORCE_BOUNDARY='1',S12_TEST_AFE='pass',S12_TEST_AFE_DELAY_FRAMES='2',S12_TEST_READY_BEAM_PRIOR='1',S12_TEST_EXPECT_INPUT_FRAMES=str(voice_end*3));env.update(extra)
        if expect_timeout:env['S12_TEST_EXPECT_ONSET_TIMEOUT']='1'
        p=subprocess.run([str(self.binary),'12000','0'],env=env,capture_output=True,timeout=8)
        self.assertEqual(p.returncode,0,(p.stdout+p.stderr).decode());events=[json.loads(x) for x in p.stdout.splitlines()]
        self.assertEqual(sum(e.get('event')=='capture_reader_started' for e in events),1);self.assertEqual(sum(e.get('event')=='capture_reader_closed' for e in events),1)
        if expect_timeout:
            self.assertTrue(any(e.get('event')=='expected_legacy_onset_timeout' for e in events));return None,events
        stats=next(e for e in events if e.get('event')=='endpoint_test_stats');self.assertGreaterEqual(stats['observed_input_frames'],voice_end*3)
        onset=next(e for e in events if e.get('event')=='speech_detected');q_start_ms=max(0,onset['onset_block']*60-onset['preroll_ms'])
        self.assertGreaterEqual(q_start_ms+stats['packets']*60,voice_end/16+60) # conservative encoded coverage beyond voice end+AFE/Opus delay
        return stats,events
    def test_generated_harmonic_bursts_keep_complete_pcm(self):
        for ms in [180,360,600]:
            with self.subTest(milliseconds=ms):
                self.capture(normalize(synthetic_voice(ms),1000))
    def test_low_snr_zero_classifier_still_preserves_asr_audio(self):
        speech=normalize(synthetic_voice(360),1000);rng=random.Random(20261025)
        noise=[rng.gauss(0,200) for _ in range(len(speech)+32000)]
        mixed=[noise[i]+(speech[i] if i<len(speech) else 0) for i in range(len(noise))]
        self.capture(mixed,voice_end=len(speech))
        stats,_=self.capture(mixed,voice_end=len(speech),S12_TEST_SPEECH_VAD='quiet')
        self.assertEqual(stats['speech_frames'],0);self.assertGreater(stats['packets'],0)
    def test_sustained_generated_voice_and_strong_prefix_not_cut(self):
        sustained=normalize(synthetic_voice(2800),1000)
        self.capture(sustained)
        prefix=normalize(synthetic_voice(180),5000)
        rng=random.Random(20261025);mixed=[x+rng.gauss(0,200) for x in sustained]
        self.capture(prefix+mixed,voice_end=len(prefix)+len(sustained))
    def test_auxiliary_error_falls_back_once_and_does_not_discard_audio(self):
        samples=normalize(synthetic_voice(600),1000)
        for fault in ['error','error-active']:
            with self.subTest(fault=fault):
                stats,events=self.capture(samples,S12_TEST_SPEECH_VAD=fault)
                self.assertEqual(sum(e.get('event')=='speech_vad_fallback' for e in events),1);self.assertEqual((stats['speech_frames'],stats['vad_frames']),(0,0))
    def test_actual_gmm_state_retained_across_relisten(self):
        p=subprocess.run([str(self.binary),'lifetime'],capture_output=True,timeout=4,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'))
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)

class CaptureStatsWireTest(unittest.TestCase):
    tearDownClass=classmethod(base.ClientTest.tearDownClass.__func__)
    invoke=base.ClientTest.invoke
    wake_env=staticmethod(base.ClientTest.wake_env)
    env=follow.FollowupTest.env
    receive=follow.FollowupTest.receive
    upload=follow.FollowupTest.upload
    meta=dialogue.DialogueTest.meta
    answer=dialogue.DialogueTest.answer
    @classmethod
    def setUpClass(cls):base.ClientTest.setUpClass.__func__(cls)
    def test_real_capture_stats_sent_once_on_matching_stop_and_error_zero_counts(self):
        for fault in [False,True]:
            with self.subTest(aux_failure=fault):
                captures=[]
                def server(p):
                    p.normal_hello();packets=self.upload(p);captures.append((p.turn_id,p.capture_stats,len(packets)));self.answer(p,[self.meta(p,'close')]);self.assertEqual(self.receive(p)[0],8)
                env=self.env();
                if fault:env['S12_TEST_SPEECH_VAD']='error'
                p=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','1'],env_change=env)
                self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(len(captures),1);turn,stats,n=captures[0];self.assertRegex(turn,r'^[0-9a-f]{32}$')
                self.assertEqual(stats['packets'],n);self.assertEqual(stats['duration_ms'],n*60);self.assertLessEqual(stats['speech_frames'],stats['vad_frames']);self.assertLessEqual(stats['vad_frames'],n*6)
                if fault:self.assertEqual((stats['speech_frames'],stats['vad_frames']),(0,0))
if __name__=='__main__':unittest.main()
