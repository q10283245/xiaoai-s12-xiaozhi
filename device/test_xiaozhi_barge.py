#!/usr/bin/env python3
"""Synthetic RAM PCM lifecycle tests. No network, hardware or actual audio.

Includes the real client and fake DSP in one native ASan/UBSan translation
unit, exercising its FIFO, AEC pump, KWS rings and continuation handoff.
"""
import json
import os
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
HARNESS = r'''
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define XIAOZHI_TEST_NO_ALSA
#define XIAOZHI_TEST_FAKE_CAPTURE
#define main client_program_main
#include "vendor_wake_test.c"
#include "xiaozhi_client.c"
#undef main
#include <assert.h>
static struct local_chain chain;
static struct barge_monitor monitor;
/* Deterministic controller for pure DSP-step tests. The production worker
 * calls this exact step; real mailbox/join/control tests live separately. */
static void drive_step(void) {
    if(monitor.candidate.valid&&(stopping||panel_interrupted||(panel_enabled&&panel.muted)))barge_candidate_clear(&monitor,"discarded");
    if(panel_enabled&&panel.muted){panel_interrupted=1;barge_active=0;return;}
    if(!barge_active||monitor.pending||monitor.failed||stopping||panel_interrupted)return;
    int result=barge_worker_step(&monitor);
    if(result==2)barge_count(&barge_status.keyword_hits); /* Test controller authorization. */
    if(result==2||result==CAPTURE_DISCONTINUITY){barge_active=0;panel_interrupted=1;}
}
static void drive_tail(void) {
    for(unsigned i=0;i<32&&monitor.candidate.valid&&barge_active&&!panel_interrupted;++i)drive_step();
}
static void prepare(void) {
    assert(!local_chain_models(&chain));assert(!local_chain_reopen(&chain));
    monitor.chain=&chain;monitor.min_rms=30;barge_select_source(&monitor);mic_frontend_init(&monitor.frontend,MIC_AUTO);
    decimator_init(&monitor.reference_filter);char error[128];
    monitor.aec=s12_vendor_aec_open(chain.wake,error,sizeof(error));assert(monitor.aec);
    monitor.reference_wake=s12_vendor_wake_open(error,sizeof(error));assert(monitor.reference_wake);s12_test_reference_wake(monitor.reference_wake);
    struct capture *c=&monitor.capture;
    c->fifo=calloc(CAPTURE_FIFO_FRAMES*CAPTURE_CHANNELS,sizeof(int32_t));assert(c->fifo);
    assert(!pthread_mutex_init(&c->mutex,NULL));c->mutex_ready=1;
    assert(!pthread_cond_init(&c->available,NULL));c->condition_ready=1;
    monitor.opened=1;barge_current=&monitor;barge_active=1;
    barge_config.reference=6;barge_config.gain=4000;
}
static void fill(unsigned blocks,int mode) {
    struct capture *c=&monitor.capture;assert(c->fifo_used+blocks*CAPTURE_CHUNK<=CAPTURE_FIFO_FRAMES);
    for(unsigned i=0;i<blocks*CAPTURE_CHUNK;++i) {
        double t=(double)(c->source_frames+i)/48000;
        unsigned index=(c->fifo_write+i)%CAPTURE_FIFO_FRAMES;
        for(unsigned ch=0;ch<CAPTURE_CHANNELS;++ch) {
            double value=5*sin(2*PI*(211+ch*31)*t);
            if(mode==1)value=(double)((int)((c->source_frames+i)%24000)-12000)+ch*17;
            if(mode==2&&ch==2&&t>=.03&&t<.75)value=700*sin(2*PI*997*t);
            if(mode==0&&ch>=6)value=0;
            if(mode==3||mode==4){value=1000*sin(2*PI*443*t);if(mode==4&&ch==2&&t>.30)value+=700*sin(2*PI*997*t);}
            c->fifo[(size_t)index*CAPTURE_CHANNELS+ch]=mode==5?(int32_t)((c->source_frames+i)*CAPTURE_CHANNELS+ch):(int32_t)lrint(value)*65536;
        }
    }
    c->source_frames+=blocks*CAPTURE_CHUNK;c->fifo_write=(c->fifo_write+blocks*CAPTURE_CHUNK)%CAPTURE_FIFO_FRAMES;
    c->fifo_used+=blocks*CAPTURE_CHUNK;
}
static void zero(const void *memory,size_t bytes) {const unsigned char *p=memory;for(size_t i=0;i<bytes;++i)assert(!p[i]);}
static void finish(void) {
    assert(!barge_stop(&monitor));assert(!barge_active&&!monitor.opened&&!monitor.aec&&!monitor.pending);
    assert(!monitor.capture.fifo&&!monitor.capture.mutex_ready&&!monitor.capture.condition_ready);
    zero(monitor.clean,sizeof(monitor.clean));zero(monitor.input,sizeof(monitor.input));
    zero(&chain.direct_ring,sizeof(chain.direct_ring));zero(chain.direct_channels,sizeof(chain.direct_channels));
    zero(&chain.hit_event,sizeof(chain.hit_event));assert(!chain.hit_source);
    uint64_t frames=chain.direct_frames;panel_interrupted=0;drive_step();assert(chain.direct_frames==frames);
    assert(!barge_stop(&monitor));barge_current=NULL;local_chain_drop(&chain);
}
int main(int argc,char **argv) {
    assert(argc>=2);local_wake_enabled=1;
    if(!strcmp(argv[1],"fifo-overflow-real")) {
        struct capture c={0};assert(!capture_open(&c));assert(!capture_start(&c));
        int64_t deadline=now_ms()+2000;int error=0;unsigned peak=0;
        while(now_ms()<deadline) {
            pthread_mutex_lock(&c.mutex);error=c.reader_error;peak=c.fifo_peak;pthread_mutex_unlock(&c.mutex);
            if(error)break;struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        }
        assert(error==CAPTURE_FIFO_OVERFLOW&&peak>96000&&peak<=144000);
        int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];memset(raw,0x5a,sizeof(raw));
        assert(capture_read(&c,raw,CAPTURE_CHUNK)==CAPTURE_FIFO_OVERFLOW);
        for(unsigned i=0;i<CAPTURE_CHUNK*CAPTURE_CHANNELS;++i)assert(raw[i]==0x5a5a5a5a);
        assert(!c.delivered_frames&&capture_finish(&c)==CAPTURE_DISCONTINUITY);
        assert(!c.fifo&&!c.reader_started&&!c.mutex_ready&&!c.condition_ready);
        assert(capture_finish(&c)==CAPTURE_DISCONTINUITY);return 0;
    }
    if(!strcmp(argv[1],"config")){assert(argc==3);return barge_load_config(argv[2])?2:0;}
    prepare();
    int ota_dir=-1;
    if(!strcmp(argv[1],"ota")){ota_dir=ota_idle_directory();assert(ota_dir>=0);assert(!renameat(ota_dir,"auto-apply.request",ota_dir,"request-held"));}
    if(!strcmp(argv[1],"status-unwritable"))barge_config.enabled=1;
    if(!strcmp(argv[1],"fifo-wrap")) {
        assert(CAPTURE_FIFO_FRAMES==144000&&CAPTURE_FIFO_FRAMES*CAPTURE_CHANNELS*sizeof(int32_t)==4608000);
        fill(40,5);assert(monitor.capture.fifo_used==115200);int32_t raw[CAPTURE_CHUNK*CAPTURE_CHANNELS];
        unsigned expected=0;
        for(unsigned block=0;block<35;++block) {
            assert(capture_read(&monitor.capture,raw,CAPTURE_CHUNK)==CAPTURE_CHUNK);
            for(unsigned i=0;i<CAPTURE_CHUNK*CAPTURE_CHANNELS;++i)assert(raw[i]==(int32_t)(expected++));
        }
        fill(40,5);assert(monitor.capture.fifo_used==129600);
        for(unsigned block=0;block<45;++block) {
            assert(capture_read(&monitor.capture,raw,CAPTURE_CHUNK)==CAPTURE_CHUNK);
            for(unsigned i=0;i<CAPTURE_CHUNK*CAPTURE_CHANNELS;++i)assert(raw[i]==(int32_t)(expected++));
        }
        assert(!monitor.capture.fifo_used&&capture_read(&monitor.capture,raw,CAPTURE_CHUNK)==-EAGAIN);
        assert(expected==230400*CAPTURE_CHANNELS);
    } else if(!strcmp(argv[1],"status")) {
        barge_config.enabled=1;barge_count(&barge_status.monitor_starts);barge_status_write("monitoring",1);
        char path[4096];assert(!panel_path(path,sizeof(path),"barge-status.json"));
        struct stat before,after;assert(!stat(path,&before));fill(4,0);drive_step();assert(!stat(path,&after));
        assert(before.st_ino==after.st_ino&&barge_status.capture_blocks==1);
        barge_status_write("interrupted",0);assert(!stat(path,&after));assert(before.st_ino!=after.st_ino);
        barge_status.written=now_ms()-1001;barge_status_write("monitoring",0);
    } else if(!strcmp(argv[1],"monitor-clocks")) {
        monitor.direct_epoch=chain.direct_frames=chain.diversity_wake->frames=400;
        monitor.beam_epoch=chain.beam_frames=chain.wake->frames=200;
        fill(8,0);for(unsigned i=0;i<8;++i)drive_step();
        assert(monitor.monitor_source==BARGE_SOURCE_DIVERSITY&&chain.direct_frames==448&&chain.beam_frames==200);
        assert(chain.diversity_wake->frames==448&&chain.wake->frames==200&&monitor.reference_frames==48);
        assert(chain.direct_ring.count==48&&!chain.beam_ring.count&&barge_model_progress(&monitor)==48);
        assert(chain.paused_models==LOCAL_PAUSED_BEAM);
        assert(!barge_stop(&monitor));
        assert(chain.paused_models==(LOCAL_PAUSED_BEAM|LOCAL_PAUSED_DIVERSITY));
        assert(!local_chain_reopen(&chain));
        assert(!chain.paused_models&&chain.wake->frames==0&&chain.beam_frames==0);
        assert(chain.direct_frames==0&&chain.diversity_wake->frames==0);
        int16_t pcm[6][UPLINK_SAMPLES];memset(pcm,0,sizeof(pcm));
        for(unsigned i=0;i<2;++i)assert(!local_chain_feed(&chain,pcm,monitor.frontend.selected,LOCAL_FEED_KWS|LOCAL_FEED_RECORD,&monitor.frontend));
        assert(chain.direct_frames==12&&chain.beam_frames==12);
        assert(chain.direct_ring.count==12&&chain.beam_ring.count==12);
        assert(local_ring_latest(&chain.direct_ring)==12&&local_ring_latest(&chain.beam_ring)==12);
    } else if(!strcmp(argv[1],"monitor-afe-fault")) {
        assert(monitor.monitor_source==BARGE_SOURCE_DIVERSITY&&chain.afe);
        fill(4,0);drive_step();
        assert(!monitor.failed&&!monitor.pending&&!barge_status.keyword_hits);
        assert(chain.direct_frames==6&&!chain.beam_frames&&monitor.monitor_source==BARGE_SOURCE_DIVERSITY);
    } else if(!strcmp(argv[1],"framing")) {
        fill(10,1);struct decimator filters[6];int16_t expected[6][10*UPLINK_SAMPLES];
        for(unsigned ch=0;ch<6;++ch){decimator_init(&filters[ch]);for(unsigned block=0;block<10;++block)
            assert(barge_mic_decimate(&filters[ch],monitor.capture.fifo+(size_t)block*CAPTURE_CHUNK*8,CAPTURE_CHUNK,ch,expected[ch]+block*UPLINK_SAMPLES)==UPLINK_SAMPLES);}
        int16_t out[6][UPLINK_SAMPLES];unsigned packets=0;
        while(!barge_next_clean(&monitor,out,0)){for(unsigned ch=0;ch<6;++ch)assert(!memcmp(out[ch],expected[ch]+packets*UPLINK_SAMPLES,UPLINK_SAMPLES*2));++packets;}
        assert(packets==10&&monitor.capture.frames==CAPTURE_CHUNK*10);
        assert(monitor.aec->blocks==75&&monitor.input_used==0&&monitor.clean_used==0);
    } else if(!strcmp(argv[1],"bounded")) {
        for(unsigned i=0;i<200;++i){fill(4,0);for(unsigned j=0;j<4;++j)drive_step();}
        assert(!monitor.pending&&!panel_interrupted&&chain.direct_frames>3200);
        assert(chain.direct_ring.count==LOCAL_RING_FRAMES&&!chain.beam_ring.count&&!chain.beam_frames);
        assert(monitor.clean_used<=UPLINK_SAMPLES&&monitor.input_used<128);
    } else if(!strcmp(argv[1],"fault")||!strcmp(argv[1],"overflow")) {
        fill(4,0);if(!strcmp(argv[1],"overflow"))monitor.capture.reader_error=CAPTURE_FIFO_OVERFLOW;
        drive_step();assert(monitor.failed==CAPTURE_DISCONTINUITY&&!barge_active&&panel_interrupted);
        assert(!monitor.pending&&!chain.direct_frames&&!chain.beam_frames);monitor.capture.reader_error=0;
    } else if(!strcmp(argv[1],"mute")) {
        fill(4,2);panel_enabled=1;panel.muted=1;drive_step();
        assert(panel_interrupted&&!barge_active&&!monitor.pending&&!chain.direct_frames&&!monitor.capture.frames);panel.muted=0;
    } else if(!strcmp(argv[1],"echo")||!strcmp(argv[1],"nearend")) {
        fill(20,!strcmp(argv[1],"echo")?3:4);for(unsigned i=0;i<20&&!monitor.pending;++i)drive_step();
        if(!strcmp(argv[1],"echo"))assert(!monitor.pending&&!panel_interrupted&&chain.direct_frames>=100);
        else assert(monitor.pending&&panel_interrupted&&!barge_active&&chain.hit_event.status==1);
    } else if(!strcmp(argv[1],"reference-repeated")) {
        for(unsigned i=0;i<UPLINK_SAMPLES;++i)monitor.block_reference[i]=1000;
        for(unsigned i=0;i<9;++i)assert(!barge_reference_feed(&monitor));
        assert(monitor.reference_frames==54&&s12_test_reference_opens==3&&barge_status.reference_hits==3);
        assert(monitor.reference_count==3&&monitor.reference_needs_reset&&!monitor.reference_model_started);
        for(unsigned i=0;i<3;++i) {
            assert(monitor.reference_events[i].input_frames==(i+1)*18);
            assert(monitor.reference_events[i].begin_frame==i*18+3&&monitor.reference_events[i].end_frame==(i+1)*18);
        }
        memset(monitor.block_reference,0,sizeof(monitor.block_reference));
        for(unsigned i=0;i<12;++i)assert(!barge_reference_feed(&monitor));
        assert(monitor.reference_frames==126&&s12_test_reference_opens==3&&monitor.reference_wake->frames==18);
        assert(monitor.reference_count==3&&barge_status.reference_hits==3);
    } else if(!strncmp(argv[1],"reference-",10)) {
        const char *mode=argv[1]+10;
        memset(monitor.block_reference,0,sizeof(monitor.block_reference));
        for(unsigned i=0;i<10;++i)assert(!barge_reference_feed(&monitor));
        assert(monitor.reference_frames==60&&!monitor.reference_wake->frames&&!monitor.reference_model_started);
        if(strcmp(mode,"silence")) {
            for(unsigned i=0;i<UPLINK_SAMPLES;++i)monitor.block_reference[i]=1000;
            for(unsigned i=0;i<3;++i)assert(!barge_reference_feed(&monitor));
            assert(monitor.reference_frames==78&&monitor.reference_wake->frames==18&&monitor.reference_model_epoch==60);
            assert(barge_status.reference_emit_frame==78&&barge_status.reference_begin_frame==63&&barge_status.reference_end_frame==78);
            memset(monitor.block_reference,0,sizeof(monitor.block_reference));
            for(unsigned i=0;i<11;++i)assert(!barge_reference_feed(&monitor));
            assert(monitor.reference_frames==144&&monitor.reference_wake->frames==18&&monitor.reference_needs_reset&&!monitor.reference_model_started);
            for(unsigned i=0;i<UPLINK_SAMPLES;++i)monitor.block_reference[i]=1000;
            for(unsigned i=0;i<3;++i)assert(!barge_reference_feed(&monitor));
            assert(monitor.reference_frames==162&&monitor.reference_wake->frames==18&&monitor.reference_model_epoch==144);
            assert(barge_status.reference_hits==2&&barge_status.reference_emit_frame==162&&barge_status.reference_begin_frame==147);
        } else assert(!barge_status.reference_dsp_ms&&!barge_status.reference_hits);
    } else if(!strncmp(argv[1],"gate-",5)) {
        const char *mode=argv[1]+5;
        if(!strcmp(mode,"beam-epochs")){monitor.monitor_source=BARGE_SOURCE_BEAM;chain.paused_models=LOCAL_PAUSED_DIVERSITY;}
        if(!strcmp(mode,"epochs")||!strcmp(mode,"beam-epochs")) {
            monitor.direct_epoch=chain.direct_frames=chain.diversity_wake->frames=1000;
            monitor.beam_epoch=chain.beam_frames=chain.wake->frames=2000;
            chain.diversity_wake->next_hit=1018;chain.wake->next_hit=2018;
        }
        fill(32,3);
        if(!strcmp(mode,"hold-overflow")||!strcmp(mode,"hold-mute")||!strcmp(mode,"hold-wall")||!strcmp(mode,"tail-accept")||!strcmp(mode,"tail-veto")) {
            for(unsigned i=0;i<8&&!monitor.candidate.valid;++i)drive_step();
            assert(monitor.candidate.valid&&!monitor.pending&&!panel_interrupted);
            if(!strcmp(mode,"hold-overflow"))monitor.capture.reader_error=CAPTURE_FIFO_OVERFLOW;
            if(!strcmp(mode,"hold-mute")){panel_enabled=1;panel.muted=1;}
            if(!strcmp(mode,"hold-wall"))monitor.candidate.wall_deadline=now_ms()-1;
            if(!strncmp(mode,"tail-",5))drive_tail();else drive_step();
        } else for(unsigned i=0;i<32&&barge_active&&!monitor.pending;++i)drive_step();
        if(!strcmp(mode,"after-veto")||!strcmp(mode,"late310")||!strcmp(mode,"tail-accept")) {
            assert(monitor.pending&&panel_interrupted&&barge_status.keyword_hits==1);
            assert(!strcmp(barge_status.last_decision,"accepted"));
            if(!strcmp(mode,"after-veto"))assert(barge_status.self_echo_vetoes>=1&&barge_status.candidate_hits==barge_status.self_echo_vetoes+1);
        } else if(!strncmp(mode,"hold-",5)) {
            assert(!monitor.pending&&!barge_status.keyword_hits&&!monitor.candidate.valid);
            assert(!strcmp(barge_status.last_decision,"discarded"));
            if(strcmp(mode,"hold-mute")&&strcmp(mode,"hold-ota"))assert(monitor.failed==CAPTURE_DISCONTINUITY);
            monitor.capture.reader_error=0;panel.muted=0;
        } else {
            assert(!monitor.pending&&!panel_interrupted&&barge_status.self_echo_vetoes>=1&&!barge_status.keyword_hits);
            assert(!strcmp(barge_status.last_decision,"self_echo"));
            assert(barge_status.candidate_emit_frame==18&&barge_status.candidate_begin_frame==3);
            assert(!chain.hit_source&&!monitor.candidate.valid&&chain.direct_frames>=18);
        }
    } else if(!strcmp(argv[1],"failure-map")) {
        fail("ALSA playback XRUN");assert(!strcmp(barge_failure,"playback")&&barge_status.playback_xruns==1);
        fail("socket read failed");assert(!strcmp(barge_failure,"network"));
        fail("server session mismatch");assert(!strcmp(barge_failure,"protocol"));
        fail("ALSA capture transfer failed");assert(!strcmp(barge_failure,"capture"));
        fail("allocation failed");assert(!strcmp(barge_failure,"other"));
    } else {
        int sentence=!strcmp(argv[1],"sentence");fill(32,sentence?2:0);
        for(unsigned i=0;i<32&&!monitor.pending;++i)drive_step();
        assert(monitor.pending&&panel_interrupted&&!barge_active&&chain.hit_event.status==1);
        assert(chain.hit_event.input_frames==18&&chain.direct_frames>=18&&chain.direct_ring.count>=18);
        uint64_t clock=chain.direct_frames;unsigned remaining_frames=monitor.capture.fifo_used;
        for(unsigned i=0;i<10;++i)drive_step();assert(chain.direct_frames==clock&&monitor.capture.fifo_used==remaining_frames);
        if(!strcmp(argv[1],"wakeonly")||sentence) {
            struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(WAKE_MAX_PACKETS,sizeof(*q.slices));assert(q.bytes&&q.slices);
            panel_interrupted=0;unsigned generation=monitor.capture.fake_generation;
            int result=local_capture_turn(&chain,&monitor.frontend,30,&q,&monitor);
            assert(result==(sentence?4:1)&&chain.direct_frames>clock);
            assert(monitor.capture.fake_generation==generation&&monitor.capture.finished&&!monitor.opened);
            if(sentence)assert(q.count>3&&q.count<WAKE_MAX_PACKETS&&q.used<=TTS_BUFFER_BYTES);else assert(!q.count&&!q.used);
            wipe(q.bytes,TTS_BUFFER_BYTES);free(q.bytes);free(q.slices);
        } else if(!strcmp(argv[1],"ota")||!strcmp(argv[1],"pending-mute")) {
            if(!strcmp(argv[1],"pending-mute")){panel_enabled=1;panel.muted=1;}else {assert(!renameat(ota_dir,"request-held",ota_dir,"auto-apply.request"));close(ota_dir);assert(ota_idle_requested());}
            assert(!barge_control_handoff(&monitor));assert(!monitor.pending&&!monitor.opened);panel.muted=0;
        } else assert(!strcmp(argv[1],"hit")||!strcmp(argv[1],"status-unwritable"));
    }
    finish();puts("barge test passed");return 0;
}
'''


class BargeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s12-barge-offline-")
        cls.base = Path(cls.temp.name)
        source = cls.base / "harness.c"
        source.write_text(HARNESS)
        cls.binary = cls.base / "barge-test"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(HERE),
                        str(source), *native_lib_flags(), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, mode, **settings):
        env = {k: v for k, v in os.environ.items() if not k.startswith("S12_TEST_")}
        env.update(S12_TEST_AFE="normal", **settings)
        result = subprocess.run([str(self.binary), mode], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def hit(self, mode, **extra):
        return self.run_case(mode, S12_TEST_LOCAL_WAKE_FRAME="18", S12_TEST_LOCAL_WAKE_BEGIN_FRAME="3",
                             S12_TEST_LOCAL_WAKE_END_FRAME="18", **extra)

    def test_framing_preserves_all_samples_and_six_channels(self): self.run_case("framing")
    def test_idle_rings_and_memory_are_bounded(self): self.run_case("bounded")
    def test_mute_precedes_keyword_processing(self): self.hit("mute")
    def test_pending_hit_discarded_on_mute(self): self.hit("pending-mute")
    def test_aec_failure_feeds_neither_partial_nor_raw_kws(self): self.hit("fault", S12_TEST_AEC_FAULT="process")
    def test_fifo_overflow_does_not_advance_kws(self): self.hit("overflow")
    def test_echo_alone_does_not_authorize_turn(self): self.hit("echo", S12_TEST_AEC_MODE="subtract", S12_TEST_LOCAL_WAKE_MIN_RMS="30")
    def test_near_end_can_interrupt_after_aec(self): self.hit("nearend", S12_TEST_AEC_MODE="subtract", S12_TEST_LOCAL_WAKE_MIN_RMS="30")

    def test_confirmed_dsp_hit_preserves_handoff_clock(self):
        self.hit("hit")

    def test_wake_only_keeps_single_capture_and_kws_clock(self):
        self.assertIn('"capture_reopened":false,"keyword_clock_preserved":true', self.hit("wakeonly"))

    def test_same_sentence_retains_bounded_confirmed_preroll(self):
        out = self.hit("sentence")
        self.assertIn('"keyword_retained":true', out)
        self.assertIn('"same_sentence":true', out)

    def test_pending_hit_closes_capture_before_ota_ack(self):
        with tempfile.TemporaryDirectory(dir=self.base) as name:
            directory = Path(name)
            directory.chmod(0o700)
            request = directory / "auto-apply.request"
            request.write_text("a" * 48 + "\n")
            request.chmod(0o600)
            self.hit("ota", S12_TEST_OTA_DIR=str(directory))

    def reference_case(self, mode, ref_frame, **settings):
        return self.hit("gate-" + mode, S12_TEST_REFERENCE_WAKE_FRAME=str(ref_frame),
                        S12_TEST_REFERENCE_BEGIN_FRAME="3", S12_TEST_REFERENCE_END_FRAME="18", **settings)

    def test_reference_hit_before_mic_vetoes_self_speech(self): self.reference_case("before", 18)
    def test_reference_hit_290ms_late_vetoes(self): self.reference_case("late290", 47)
    def test_reference_hit_310ms_late_cannot_revoke_accepted_turn(self): self.reference_case("late310", 49)
    def test_persistent_direct_epoch_maps_to_fresh_reference_clock(self): self.reference_case("epochs", 18)
    def test_persistent_beam_epoch_maps_to_fresh_reference_clock(self): self.reference_case("beam-epochs", 18, S12_TEST_LOCAL_WAKE_SOURCE="beam")
    def test_veto_does_not_block_next_independent_wake(self): self.reference_case("after-veto", 18, S12_TEST_LOCAL_WAKE_EVERY="60", S12_TEST_REFERENCE_HIT_COUNT="1")
    def test_reference_kws_failure_during_hold_discards_candidate(self): self.reference_case("hold-reference", 0, S12_TEST_REFERENCE_FAULT_FRAME="24")
    def test_aec_failure_during_hold_discards_candidate(self): self.reference_case("hold-aec", 0, S12_TEST_AEC_FAULT="process", S12_TEST_AEC_FAIL_AFTER="32")
    def test_buffered_clean_pcm_cannot_bypass_latched_overflow(self): self.reference_case("hold-overflow", 0)
    def test_mute_during_hold_discards_candidate(self): self.reference_case("hold-mute", 0)
    def test_hard_wall_expiry_discards_instead_of_accepting(self): self.reference_case("hold-wall", 0)
    def test_normal_media_eof_confirms_pending_real_wake(self): self.reference_case("tail-accept", 0)
    def test_normal_media_eof_waits_for_reference_veto(self): self.reference_case("tail-veto", 47)
    def test_classified_failures_are_fixed_aggregates(self): self.run_case("failure-map")

    def test_exact_zero_reference_advances_only_physical_clock(self):
        self.run_case("reference-silence", S12_TEST_REFERENCE_FAULT_FRAME="1")

    def test_reference_gap_restarts_real_model_and_maps_new_epoch(self):
        self.run_case("reference-gap", S12_TEST_REFERENCE_WAKE_FRAME="18", S12_TEST_REFERENCE_BEGIN_FRAME="3", S12_TEST_REFERENCE_END_FRAME="18")

    def test_each_reference_hit_rearms_without_replaying_or_losing_history(self):
        self.run_case("reference-repeated", S12_TEST_REFERENCE_WAKE_FRAME="18", S12_TEST_REFERENCE_BEGIN_FRAME="3", S12_TEST_REFERENCE_END_FRAME="18")

    def test_three_second_fifo_wrap_preserves_all_channels_without_prefill(self):
        self.run_case("fifo-wrap")

    def test_real_reader_exceeding_fixed_three_second_cap_fails_closed(self):
        self.run_case("fifo-overflow-real", S12_TEST_CAPTURE_FAST="1")

    def test_monitor_gain_epoch_rebuilds_both_clocks_before_ordinary_capture(self):
        self.run_case("monitor-clocks")

    def test_unused_afe_failure_does_not_break_fixed_diversity_monitor(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("S12_TEST_")}
        env["S12_TEST_AFE"] = "broken"
        result = subprocess.run([str(self.binary), "monitor-afe-fault"], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_status_is_flat_private_throttled_and_monotonic(self):
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            self.run_case("status", S12_TEST_PANEL_DIR=directory)
            path = Path(directory) / "barge-status.json"
            status = json.loads(path.read_text())
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(status["schema"], 3)
            self.assertIs(status["enabled"], True)
            self.assertEqual(status["state"], "idle")  # verified final capture close
            self.assertEqual(status["monitor_starts"], 1)
            self.assertEqual(status["capture_blocks"], 1)
            self.assertEqual(status["reference_channel"], 6)
            self.assertEqual(status["reference_gain_permille"], 4000)
            self.assertEqual(set(status), {"schema", "enabled", "state", "monitor_starts", "capture_blocks",
                "keyword_hits", "capture_discontinuities", "old_turns_dropped", "reference_channel",
                "reference_gain_permille", "last_dsp_ms", "max_dsp_ms", "candidate_hits", "reference_hits", "self_echo_vetoes",
                "last_decision", "candidate_begin_frame", "candidate_end_frame", "candidate_emit_frame", "reference_begin_frame",
                "reference_end_frame", "reference_emit_frame", "last_turn_error", "playback_xruns", "capture_backlog_frames",
                "reference_dsp_ms", "playback_buffer_frames", "playback_period_frames", "conversation_active", "followup_windows", "followup_turns", "last_error_reason"})
            self.assertTrue(all(isinstance(value, (int, str)) for value in status.values()))
            self.assertLessEqual(status["last_dsp_ms"], status["max_dsp_ms"])
            self.assertLessEqual(status["max_dsp_ms"], 10000)
            self.assertEqual(list(Path(directory).glob("*client-new*")), [])

    def test_status_write_failure_does_not_stop_keyword_interruption(self):
        self.hit("status-unwritable", S12_TEST_PANEL_DIR=str(self.base / "not-a-directory"))

    def test_only_matching_private_boot_verification_enables_barge(self):
        good = dict(schema=1, reference_verified=True, capture_rate=48000, capture_channels=8,
                    ref_channel=6, ref_delay_samples=0, ref_gain_permille=1000, device="S12A",
                    vendor_sha256="79f1a33d9683cd6940c8d23e3dd4e992f1958fe220c0dfff8d230f9f8a1b5e73", source_image="a" * 64)
        config, marker = self.base / "config.json", self.base / "marker.json"
        env = dict(os.environ, S12_TEST_BARGE_MARKER=str(marker))
        def run(value, expected, marker_value=None, mode=0o600):
            config.write_text(json.dumps(value));config.chmod(mode)
            marker.write_text(json.dumps(value if marker_value is None else marker_value));marker.chmod(0o600)
            result = subprocess.run([str(self.binary), "config", str(config)], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        run(good, 0)
        for key, value in [("reference_verified", False), ("reference_verified", 1), ("ref_channel", 0),
                           ("ref_delay_samples", 1), ("ref_gain_permille", 4001), ("source_image", "bad"),
                           ("vendor_sha256", "0" * 64), ("device", "ESP32")]:
            run(dict(good, **{key: value}), 2)
        run(good, 2, marker_value=dict(good, ref_channel=7))
        run(good, 2, mode=0o644)
        marker.unlink();marker.symlink_to(config)
        result = subprocess.run([str(self.binary), "config", str(config)], env=env, capture_output=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__": unittest.main()
