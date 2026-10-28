#!/usr/bin/env python3
"""Exact production music receiver/Opus tests; native memory only, no I/O."""
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import json
import os
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

PREFIX = r'''
#define XIAOZHI_TEST_NO_ALSA 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaozhi_json_abi.h"
typedef struct OpusDecoder OpusDecoder;
typedef struct OpusEncoder OpusEncoder;
extern OpusDecoder *opus_decoder_create(int,int,int *);
extern void opus_decoder_destroy(OpusDecoder *);
extern int opus_decode(OpusDecoder *,const unsigned char *,int,int16_t *,int,int);
extern OpusEncoder *opus_encoder_create(int,int,int,int *);
extern void opus_encoder_destroy(OpusEncoder *);
extern int opus_encode(OpusEncoder *,const int16_t *,int,unsigned char *,int);
static size_t live,peak;
static void *tracked_malloc(size_t n){size_t *p=malloc(n+sizeof(size_t));if(!p)return NULL;*p=n;live+=n;if(live>peak)peak=live;return p+1;}
static void *tracked_calloc(size_t n,size_t k){void *p=tracked_malloc(n*k);if(p)memset(p,0,n*k);return p;}
static void tracked_free(void *raw){if(raw){size_t *p=(size_t *)raw-1;live-=*p;free(p);}}
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
static int stopping,panel_interrupted,local_wake_enabled;
static unsigned followup_seconds;
static struct {unsigned active;} ui_feedback;
static int64_t clock_ms=1000;
static char failure[100];
static int fail(const char *s){snprintf(failure,sizeof(failure),"%s",s);return -1;}
static int64_t now_ms(void){return clock_ms;}
static int remaining(int64_t end){return end>clock_ms?(int)(end-clock_ms):0;}
static const char *mode;
static unsigned sent,step,target,pre_stop_packets;
static uint64_t pre_stop_frames;
static size_t stream_live_max;
static void panel_state(const char *s){(void)s;}
static void panel_poll(void){if(sent==30&&(!strcmp(mode,"cancel")||!strcmp(mode,"mute")))panel_interrupted=1;if(sent==30&&!strcmp(mode,"term"))stopping=1;}
static unsigned char packet[4096];static int packet_len;
'''

MOCK = r'''
static int device_mcp_receive(struct wire *w,struct json_object *j){(void)w;(void)j;return 0;}
static struct audio *active;
static int text(struct message *m,const char *s){m->opcode=1;m->len=strlen(s);memcpy(m->data,s,m->len);return 0;}
static int receive_message(struct wire *w,struct message *m){
 if(stopping||panel_interrupted)return -1;
 if(active->music){assert(active->packets==sent);assert(active->samples==(uint64_t)sent*2880);assert(live<100000);if(live>stream_live_max)stream_live_max=live;}
 if(!strcmp(mode,"outside")&&step++==0)return text(m,"{\"type\":\"music\",\"state\":\"playing\",\"session_id\":\"session\"}");
 if(step==0){step++;return text(m,"{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"session\"}");}
 if(step==1){
  step++;
  if(!strncmp(mode,"regular",7))return receive_message(w,m);
  if(!strcmp(mode,"late")){m->opcode=2;m->len=packet_len;memcpy(m->data,packet,packet_len);return 0;}
  if(!strcmp(mode,"missing-session"))return text(m,"{\"type\":\"music\",\"state\":\"playing\"}");
  if(!strcmp(mode,"empty-session"))return text(m,"{\"type\":\"music\",\"state\":\"playing\",\"session_id\":\"\"}");
  if(!strcmp(mode,"wrong-session"))return text(m,"{\"type\":\"music\",\"state\":\"playing\",\"session_id\":\"other\"}");
  return text(m,"{\"type\":\"music\",\"state\":\"playing\",\"session_id\":\"session\"}");
 }
 if(!strcmp(mode,"late")||(!strcmp(mode,"duplicate")&&sent==10))return text(m,"{\"type\":\"music\",\"state\":\"playing\",\"session_id\":\"session\"}");
 if(!strcmp(mode,"duplicate-tts")&&sent==10)return text(m,"{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"session\"}");
 if(!strcmp(mode,"network")&&sent==30)return fail("fixture network failure");
 if(!strcmp(mode,"stall")&&sent==1)clock_ms+=15001;
 else clock_ms+=!strcmp(mode,"regular-long")?1:!strcmp(mode,"hardcap")?70:60;
 if(!remaining(w->deadline))return fail("fixture read deadline expired");
 if(!strcmp(mode,"lyric-stall")&&sent==1)return text(m,"{\"type\":\"music\",\"state\":\"lyric\",\"text\":\"synthetic\",\"session_id\":\"session\"}");
 if(sent<target){m->opcode=2;m->len=!strcmp(mode,"empty-packet")?0:(size_t)packet_len;memcpy(m->data,packet,m->len);if(!strcmp(mode,"bad-packet")){m->len=2;m->data[0]=3;m->data[1]=0;}sent++;return 0;}
 pre_stop_packets=active->packets;pre_stop_frames=active->samples;
 return text(m,"{\"type\":\"tts\",\"state\":\"stop\",\"session_id\":\"session\"}");
}
'''

MAIN = r'''
int main(int argc,char **argv){
 assert(argc==2);mode=argv[1];target=!strcmp(mode,"long")||!strcmp(mode,"regular-long")?2200:!strcmp(mode,"limit")?12017:!strcmp(mode,"max")?12016:!strcmp(mode,"hardcap")?12016:!strcmp(mode,"regular")?8:100;
 int error=0;OpusEncoder *e=opus_encoder_create(16000,1,2049,&error);assert(e&&!error);int16_t pcm[960];for(unsigned i=0;i<960;i++)pcm[i]=(int16_t)((i%79)*200-7900);packet_len=opus_encode(e,pcm,960,packet,sizeof(packet));assert(packet_len>0);opus_encoder_destroy(e);
 struct wire w={.fd=-1,.deadline=clock_ms+120000};struct audio a={.buffer_tts=1};active=&a;
 int rc=0;
 if(!strcmp(mode,"normal-limit")){for(unsigned i=0;i<10001&&!rc;i++)rc=decode_audio(&a,packet,packet_len,w.deadline);}
 else rc=receive_turn(&w,"session",&a,0,1);
 unsigned packets=a.packets;uint64_t frames=a.samples;int music=a.music;close_audio(&a);assert(live==0);
 unsigned saved_pre=pre_stop_packets;uint64_t saved_frames=pre_stop_frames;int64_t saved_clock=clock_ms;char saved_failure[100];strcpy(saved_failure,failure);
 int recovered=0;
 if(rc<0){mode="regular";target=8;sent=step=0;stopping=panel_interrupted=0;memset(&a,0,sizeof(a));a.buffer_tts=1;w.deadline=clock_ms+120000;assert(receive_turn(&w,"session",&a,0,1)==0);assert(!a.music&&a.packets==8&&a.samples==8*2880);close_audio(&a);assert(live==0);recovered=1;}
 printf("{\"rc\":%d,\"packets\":%u,\"frames\":%llu,\"music\":%d,\"pre_stop_packets\":%u,\"pre_stop_frames\":%llu,\"peak\":%llu,\"stream_live_max\":%llu,\"clock\":%lld,\"failure\":\"%s\",\"fresh_turn_recovered\":%d}\n",rc,packets,(unsigned long long)frames,music,saved_pre,(unsigned long long)saved_frames,(unsigned long long)peak,(unsigned long long)stream_live_max,(long long)saved_clock,saved_failure,recovered);
 return 0;
}
'''


class MusicReceiverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s12-music-native-")
        root = Path(cls.temp.name)
        source = (HERE / "xiaozhi_client.c").read_text()
        constants = "\n".join(line for line in source.splitlines() if line.startswith("#define ") and any(line.startswith("#define " + n + " ") for n in ["MAX_JSON", "MAX_OPUS", "MAX_AUDIO_PACKETS", "MAX_MUSIC_AUDIO_PACKETS", "MUSIC_IDLE_TIMEOUT_MS", "MUSIC_HARD_TIMEOUT_MS", "TTS_BUFFER_BYTES", "TTS_BUFFER_PACKETS", "DECODE_FRAMES"])) + "\n"
        structs = source[source.index("struct mcp_cached_volume {"):source.index("/* Errors contain", source.index("struct wire {"))]
        parser = source[source.index("static int utf8_valid("):source.index("static int copy_field(")]
        dialogue = source[source.index("static int dialogue_flat_keys("):source.index("/* A prompt is consumed only after a completed stop-only turn.")]
        decoder = source[source.index("static int decode_audio("):source.index("static unsigned farewell_last=")]
        queue = source[source.index("struct opus_slice {"):source.index("/* VAD only rejects")]
        receiver = source[source.index("static int receive_turn("):source.index("static int receive_tts(")]
        path=root/"music.c";path.write_text(PREFIX+constants+structs+parser+dialogue+decoder+queue+MOCK+receiver+MAIN)
        cls.binary=root/"music"
        subprocess.run(["clang","-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I"+str(HERE),str(path),*native_lib_flags(),"-o",str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, mode):
        r=subprocess.run([str(self.binary),mode],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"),timeout=15)
        self.assertEqual(r.returncode,0,r.stderr)
        return json.loads(r.stdout.splitlines()[-1])

    def test_regular_tts_keeps_whole_turn_buffer(self):
        r=self.run_case("regular");self.assertEqual((r["rc"],r["music"],r["packets"],r["pre_stop_frames"]),(0,0,8,0))

    def test_music_decodes_before_stop_and_beyond_old_2000_queue(self):
        r=self.run_case("long");self.assertEqual((r["rc"],r["pre_stop_packets"],r["pre_stop_frames"]),(0,2200,2200*2880));self.assertLess(r["stream_live_max"],100000);self.assertLess(r["peak"],700000)

    def test_full_backend_cap_fits_and_next_packet_fails(self):
        r=self.run_case("max");self.assertEqual((r["rc"],r["packets"]),(0,12016))
        r=self.run_case("limit");self.assertEqual(r["rc"],-1);self.assertEqual(r["frames"],12016*2880);self.assertIn("packet limit",r["failure"])

    def test_regular_packet_limit_and_whole_turn_queue_limit_preserved(self):
        r=self.run_case("normal-limit");self.assertEqual((r["rc"],r["frames"]),(-1,10000*2880));self.assertIn("packet limit",r["failure"])
        r=self.run_case("regular-long");self.assertEqual((r["rc"],r["frames"]),(-1,0));self.assertIn("RAM buffer limit",r["failure"])

    def test_marker_order_and_explicit_matching_session(self):
        for mode in ["outside","late","duplicate","duplicate-tts","missing-session","empty-session","wrong-session"]:
            with self.subTest(mode=mode):self.assertEqual(self.run_case(mode)["rc"],-1)

    def test_empty_audio_fails_without_decoding(self):
        r=self.run_case("empty-packet");self.assertEqual((r["rc"],r["frames"]),(-1,0))

    def test_invalid_opus_fails_and_fresh_turn_recovers(self):
        r=self.run_case("bad-packet");self.assertEqual((r["rc"],r["frames"],r["fresh_turn_recovered"]),(-1,0,1))

    def test_audio_stall_and_lyrics_cannot_refresh_idle_deadline(self):
        for mode in ["stall","lyric-stall"]:
            r=self.run_case(mode);self.assertEqual((r["rc"],r["packets"]),(-1,1));self.assertLess(r["clock"],17000)

    def test_fixed_hard_deadline_does_not_slide_with_audio(self):
        r=self.run_case("hardcap");self.assertEqual(r["rc"],-1);self.assertGreater(r["packets"],2000);self.assertLessEqual(r["clock"],781070)

    def test_cancel_mute_and_term_close_decoder_and_receiver_allocations(self):
        for mode in ["cancel","mute","term","network"]:
            r=self.run_case(mode);self.assertEqual((r["rc"],r["packets"],r["fresh_turn_recovered"]),(-1,30,1))


if __name__ == "__main__":
    unittest.main()
