#!/usr/bin/env python3
"""Exercise the production prompt/write/drain code against a native ALSA sink.

Synthetic PCM and the generated mathematical boot cue are read into a native
memory sink. No audio device, serial port, network, credential store or ARM
code is executed; nothing is audibly played or uploaded.
"""
from pathlib import Path
import json
import importlib.util
import os
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def production_functions():
    source = (HERE / "xiaozhi_client.c").read_text()
    audio = source[source.index("struct audio {"):source.index("\n};", source.index("struct audio {")) + 3]
    playback = source[source.index("static int open_playback("):source.index("\n#endif\nstatic int decode_audio(")]
    close = source[source.index("static void close_audio("):source.index("static int local_prompt(")]
    prompt = source[source.index("static int local_prompt("):source.index("/* Called only with every capture reader/DSP worker joined and closed.")]
    return audio, playback, close, prompt


PREFIX = r"""
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "followup_ready_pcm.h"
#include "ui_feedback_pcm.h"
#include "farewell_prompt_pcm.h"
#include <unistd.h>
#include "alsa_pcm_abi.h"
typedef void OpusDecoder;
#define DECODE_FRAMES 5760
struct _snd_pcm { int state; };
struct _snd_pcm_hw_params { int unused; };
struct _snd_pcm_sw_params { int unused; };
struct backend {
    int mode,opened,closed,dropped,drains,queries,writes,waits,last_fd;
    int stop_before_write,cancel_before_write,short_writes;
    unsigned period,buffer;
    uint64_t frames,body;
    int64_t clock_extra;
    snd_pcm_uframes_t start_threshold;
    int32_t output[520000];
};
static struct backend b;
static unsigned prompt_random;
static int random_bytes(uint8_t *out,size_t n) {memset(out,0,n);memcpy(out,&prompt_random,n<sizeof(prompt_random)?n:sizeof(prompt_random));return 0;}
static struct _snd_pcm device;
static int stopping,panel_interrupted,panel_enabled,barge_active;
static void barge_playback_settings(unsigned buffer,unsigned period) {(void)buffer;(void)period;}
static struct { unsigned gain; } panel;
static struct {unsigned initialized,pending,active,offset;int64_t first,updated,last;} ui_feedback;
static void ui_feedback_overlay(int32_t *out,int n,int gain) {(void)out;(void)n;(void)gain;}
static const char *input_root;
static int64_t now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000+b.clock_extra;}
static int remaining(int64_t deadline) {int64_t n=deadline-now_ms();return n>0?(int)n:0;}
static void panel_poll(void) {}
static void wipe(void *p,size_t n) {memset(p,0,n);}
static int fail(const char *message) {(void)message;return -1;}
static void opus_decoder_destroy(OpusDecoder *decoder) {(void)decoder;assert(!"unexpected decoder");}
static int metadata_stat(int fd,struct stat *st) {
    int rc=fstat(fd,st);
    if(b.stop_before_write)stopping=1;
    if(b.cancel_before_write)panel_interrupted=1;
    return rc;
}
static int prompt_open(const char *path,int flags) {
    char mapped[4096];const char *name=strrchr(path,'/');assert(name);
    assert(snprintf(mapped,sizeof(mapped),"%s%s",input_root,name)<(int)sizeof(mapped));
    b.last_fd=open(mapped,flags);return b.last_fd;
}
int snd_pcm_open(snd_pcm_t **out,const char *name,int direction,int flags) {
    assert(!strcmp(name,"hw:0,1")&&direction==S12_PLAYBACK&&flags==S12_NONBLOCK);
    ++b.opened;device.state=0;*out=&device;return 0;
}
int snd_pcm_close(snd_pcm_t *pcm) {assert(pcm==&device&&b.dropped==1);++b.closed;return 0;}
int snd_pcm_drop(snd_pcm_t *pcm) {assert(pcm==&device&&!b.closed);++b.dropped;device.state=S12_STATE_SETUP;return 0;}
int snd_pcm_prepare(snd_pcm_t *pcm) {assert(pcm==&device);device.state=S12_STATE_PREPARED;return 0;}
int snd_pcm_state(snd_pcm_t *pcm) {assert(pcm==&device);return device.state;}
int snd_pcm_wait(snd_pcm_t *pcm,int ms) {assert(pcm==&device&&ms==20);++b.waits;return 1;}
int snd_pcm_get_params(snd_pcm_t *pcm,snd_pcm_uframes_t *buffer,snd_pcm_uframes_t *period) {
    assert(pcm==&device&&b.opened&&!b.closed);++b.queries;
    if(b.mode==3)return -EIO;
    *buffer=b.buffer;*period=b.period;return 0;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm,const void *data,snd_pcm_uframes_t frames) {
    assert(pcm==&device&&frames>0&&frames<=5760&&b.opened&&!b.closed);++b.writes;
    if(b.mode==4&&b.frames==b.body)return -EPIPE;
    if(b.short_writes&&b.writes%3==0)return -EAGAIN;
    unsigned count=(unsigned)frames;
    if(b.short_writes&&count>257)count=257;
    if(b.mode==5&&b.frames>=b.body&&count>64)count=64;
    assert((b.frames+count)*2<sizeof(b.output)/sizeof(b.output[0]));
    memcpy(b.output+b.frames*2,data,(size_t)count*8);b.frames+=count;
    if(device.state==S12_STATE_PREPARED&&b.frames>=b.start_threshold)device.state=S12_STATE_RUNNING;
    if(b.mode==5&&b.frames>b.body)panel_interrupted=1;
    if(b.mode==6&&b.frames>=b.body)b.clock_extra=9000;
    return (long)count;
}
int snd_pcm_drain(snd_pcm_t *pcm) {
    assert(pcm==&device&&!b.dropped&&!b.closed);++b.drains;
    if(b.mode==12&&device.state==S12_STATE_PREPARED)return -EINVAL;
    if(b.mode==7&&b.drains<=2){device.state=S12_STATE_DRAINING;return -EAGAIN;}
    if(b.mode==8){device.state=S12_STATE_DRAINING;panel_interrupted=1;return -EAGAIN;}
    device.state=S12_STATE_SETUP;return 0;
}
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **p) {*p=calloc(1,sizeof(**p));return *p?0:-ENOMEM;}
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *p) {free(p);}
int snd_pcm_hw_params_any(snd_pcm_t *p,snd_pcm_hw_params_t *h) {(void)p;(void)h;return 0;}
int snd_pcm_hw_params_set_access(snd_pcm_t *p,snd_pcm_hw_params_t *h,int v) {(void)p;(void)h;assert(v==S12_RW_INTERLEAVED);return 0;}
int snd_pcm_hw_params_set_format(snd_pcm_t *p,snd_pcm_hw_params_t *h,int v) {(void)p;(void)h;assert(v==S12_S32_LE);return 0;}
int snd_pcm_hw_params_set_channels(snd_pcm_t *p,snd_pcm_hw_params_t *h,unsigned v) {(void)p;(void)h;assert(v==2);return 0;}
int snd_pcm_hw_params_set_rate(snd_pcm_t *p,snd_pcm_hw_params_t *h,unsigned v,int d) {(void)p;(void)h;(void)d;assert(v==48000);return 0;}
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *p,snd_pcm_hw_params_t *h,snd_pcm_uframes_t *v,int *d) {(void)p;(void)h;(void)d;assert(*v==960);*v=b.period;return 0;}
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *p,snd_pcm_hw_params_t *h,snd_pcm_uframes_t *v) {(void)p;(void)h;assert(*v==15360);return 0;}
int snd_pcm_hw_params(snd_pcm_t *p,snd_pcm_hw_params_t *h) {(void)p;(void)h;return 0;}
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *h,int *v) {(void)h;*v=S12_S32_LE;return 0;}
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *h,unsigned *v) {(void)h;*v=2;return 0;}
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *h,unsigned *v,int *d) {(void)h;(void)d;*v=48000;return 0;}
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **p) {*p=calloc(1,sizeof(**p));return *p?0:-ENOMEM;}
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *p) {free(p);}
int snd_pcm_sw_params_current(snd_pcm_t *p,snd_pcm_sw_params_t *h) {(void)p;(void)h;return 0;}
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *p,snd_pcm_sw_params_t *h,snd_pcm_uframes_t v) {(void)p;(void)h;b.start_threshold=v;return 0;}
int snd_pcm_sw_params(snd_pcm_t *p,snd_pcm_sw_params_t *h) {(void)p;(void)h;return 0;}
#define open prompt_open
"""

MAIN = r"""
#undef open
int main(int argc,char **argv) {
    assert(argc==6||argc==7);if(argc==7)prompt_random=(unsigned)strtoul(argv[6],NULL,10);input_root=argv[1];const char *name=argv[2];b.mode=atoi(argv[3]);
    b.period=(unsigned)strtoul(argv[4],NULL,10);b.buffer=15360;b.last_fd=-1;
    b.short_writes=atoi(argv[5]);b.stop_before_write=b.mode==9;b.cancel_before_write=b.mode==10;
    unsetenv("S12_PLAY_PCM");
    const char *file=!strcmp(name,"wake")?"wakeup.pcm":!strcmp(name,"boot")?"bootup.pcm":!strcmp(name,"wifi")?"wifi-unavailable.pcm":"cloud-unavailable.pcm";
    int embedded=!strcmp(name,"followup")||!strcmp(name,"volume")||!strcmp(name,"button")||!strcmp(name,"command-key")||!strcmp(name,"farewell");long bytes=0;int16_t *original=NULL;
    if(embedded) {
        const unsigned char *pcm=!strcmp(name,"followup")?s12_followup_prompt_pcm:!strcmp(name,"volume")?s12_volume_prompt_pcm:s12_button_prompt_pcm;
        bytes=!strcmp(name,"followup")?(long)S12_FOLLOWUP_PROMPT_PCM_BYTES:!strcmp(name,"volume")?(long)S12_VOLUME_PROMPT_PCM_BYTES:(long)S12_BUTTON_PROMPT_PCM_BYTES;
        if(!strcmp(name,"farewell")){unsigned clip=prompt_random%S12_FAREWELL_PROMPT_COUNT;pcm=s12_farewell_clips[clip].pcm;bytes=(long)s12_farewell_clips[clip].bytes;}
        original=malloc((size_t)bytes+2);assert(original);memcpy(original,pcm,(size_t)bytes);
        if(b.stop_before_write)stopping=1;
        if(b.cancel_before_write)panel_interrupted=1;
    } else {
        char path[4096];assert(snprintf(path,sizeof(path),"%s/%s",input_root,file)<(int)sizeof(path));
        FILE *f=fopen(path,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);bytes=ftell(f);rewind(f);
        original=malloc((size_t)bytes+2);assert(original);assert(fread(original,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    }
    b.body=(uint64_t)bytes/2*3;
    struct audio options;memset(&options,0,sizeof(options));options.gain=200;
    int rc=local_prompt(name,&options);
    int success=!b.mode||b.mode==7||b.mode==12;
    if(success) {
        assert(rc==0&&b.drains==(b.mode==7?3:1)&&b.opened==1&&b.dropped==1&&b.closed==1);
        int align=!strcmp(name,"wake")||!strcmp(name,"boot")||embedded;
        uint64_t pad=align?(b.period-b.body%b.period)%b.period:0;
        assert(b.frames==b.body+pad&&b.start_threshold==(!strcmp(name,"followup")?b.period:11520));
        assert(b.queries==(align?1:0));
        for(uint64_t i=0;i<b.body;++i) {
            int32_t expected=(int32_t)((int64_t)original[i/3]*200*65536/1000);
            assert(b.output[i*2]==expected&&b.output[i*2+1]==expected);
        }
        for(uint64_t i=b.body;i<b.frames;++i)assert(!b.output[i*2]&&!b.output[i*2+1]);
        if(b.short_writes)assert(b.waits>0&&b.writes>4);
    } else {
        assert(rc<0);
        if(b.mode==9||b.mode==10)assert(!b.opened&&!b.queries&&!b.writes&&!b.drains&&!b.dropped&&!b.closed);
        else assert(b.opened==1&&b.dropped==1&&b.closed==1);
        if(b.mode!=8)assert(!b.drains);
        if(b.mode==5)assert(b.frames==b.body+64);
        if(b.mode==6)assert(b.frames==b.body);
    }
    if(embedded)assert(b.last_fd==-1);
    else assert(b.last_fd>=0&&fcntl(b.last_fd,F_GETFD)==-1&&errno==EBADF);
    printf("{\"verified\":true,\"frames\":%llu,\"queries\":%d,\"writes\":%d,\"waits\":%d,\"closed\":%d}\n",(unsigned long long)b.frames,b.queries,b.writes,b.waits,b.closed);
    free(original);wipe(b.output,sizeof(b.output));return 0;
}
"""


class PromptPlaybackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s12-prompt-offline-")
        cls.root = Path(cls.temp.name)
        audio, playback, close, prompt = production_functions()
        source = cls.root / "prompt.c"
        source.write_text(PREFIX + audio + "\n" + playback + "\n" + close + prompt + MAIN)
        cls.binary = cls.root / "prompt-native-sink"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(HERE), str(source), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name="wake", mode=0, period=960, short=False, samples=5538, body=None, farewell_index=None):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            directory = Path(td)
            if body is None:
                body = b"".join(struct.pack("<h", ((i * 7919) % 50001) - 25000) for i in range(samples))
            for filename in ["wakeup.pcm", "bootup.pcm", "wifi-unavailable.pcm", "cloud-unavailable.pcm"]:
                file = directory / filename
                file.write_bytes(body)
                file.chmod(0o600)
            command=[str(self.binary),td,name,str(mode),str(period),str(int(short))]
            if farewell_index is not None:command.append(str(farewell_index))
            r = subprocess.run(command,
                               capture_output=True, text=True, timeout=4,
                               env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            result = json.loads(r.stdout.splitlines()[-1])
            self.assertTrue(result["verified"])
            return result

    def test_farewell_complete_pcm_each_clip_and_command_key_eof(self):
        for index in range(3):
            with self.subTest(clip=index):
                result=self.run_case(name="farewell",short=True,farewell_index=index)
                self.assertEqual(result["queries"],1)
        self.assertEqual(self.run_case(name="command-key",short=True)["frames"],2880)

    def test_exact_body_stereo_gain_and_actual_period_padding(self):
        for period, frames in [(960, 17280), (512, 16896), (8192, 24576)]:
            with self.subTest(period=period):
                self.assertEqual(self.run_case(period=period)["frames"], frames)

    def test_already_aligned_body_has_no_added_frames(self):
        self.assertEqual(self.run_case(samples=640)["frames"], 1920)

    def test_partial_writes_and_eagain_keep_exact_order_without_replay(self):
        self.assertEqual(self.run_case(short=True)["frames"], 17280)

    def test_other_local_prompts_do_not_query_or_pad(self):
        for name in ["wifi", "cloud"]:
            with self.subTest(name=name):
                result = self.run_case(name=name)
                self.assertEqual(result["frames"], 16614)
                self.assertEqual(result["queries"], 0)

    def test_query_failure_invalid_period_and_eof_write_error_close(self):
        for mode, period in [(3, 960), (11, 0), (11, 20000), (4, 960)]:
            with self.subTest(mode=mode, period=period):
                self.assertEqual(self.run_case(mode=mode, period=period)["closed"], 1)

    def test_cancel_term_before_first_write_never_query_null_pcm(self):
        for mode in [9, 10]:
            with self.subTest(mode=mode):
                result = self.run_case(mode=mode)
                self.assertEqual(result["queries"], 0)
                self.assertEqual(result["closed"], 0)

    def test_cancel_and_deadline_during_eof_drop_without_drain(self):
        for mode in [5, 6]:
            with self.subTest(mode=mode):
                self.assertEqual(self.run_case(mode=mode)["closed"], 1)

    def test_nonblocking_drain_retry_and_cancel_keep_close_order(self):
        self.run_case(mode=7)
        self.assertEqual(self.run_case(mode=8)["closed"], 1)

    def test_boot_exact_length_actual_period_and_partial_writes(self):
        self.assertEqual(self.run_case(name="boot",samples=59904,short=True)["frames"],180480)

    def test_synthetic_boot_pcm_is_preserved_value_for_value(self):
        path=HERE.parent/"scripts/generate_prompts.py"
        specification=importlib.util.spec_from_file_location("original_prompt_recipe",path)
        module=importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        body=module.pcm([(660,120),(0,35),(880,120),(0,35),(1320,140)])
        self.assertEqual(self.run_case(name="boot",body=body)["frames"],22080)

    def test_boot_cancel_error_deadline_and_pre_stop_share_safe_cleanup(self):
        for mode in [3,4,5,6,8,9,10]:
            with self.subTest(mode=mode):
                self.run_case(name="boot",mode=mode,samples=59904)

    def test_short_followup_is_running_before_drain_without_trailing_fill(self):
        result=self.run_case(name="followup",mode=12,period=960,short=True)
        self.assertEqual(result['frames'],4800)

    def test_followup_short_derived_pcm_gain_stereo_and_period_alignment(self):
        for period,frames in [(960,4800),(512,5120),(8192,8192)]:
            with self.subTest(period=period):
                result=self.run_case(name="followup",period=period,short=True)
                self.assertEqual(result["frames"],frames)
                self.assertEqual(result["closed"],1)

    def test_volume_button_full_embedded_body_and_same_period_eof(self):
        for name,frames in [("volume",3840),("button",2880)]:
            with self.subTest(name=name):
                self.assertEqual(self.run_case(name=name,short=True)["frames"],frames)

    def test_followup_embedded_cancel_error_timeout_and_drain_cleanup(self):
        for mode in [3,4,5,6,7,8,9,10]:
            with self.subTest(mode=mode):
                self.run_case(name="followup",mode=mode,period=512 if mode in [4,5,6] else 960)


if __name__ == "__main__":
    unittest.main()
