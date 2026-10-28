#!/usr/bin/env python3
"""Production UI mixer/queue against a native ALSA memory sink.

No target device, microphone, serial port or network is opened.  The actual
queue, overlay, write and drain functions are extracted from the client so the
sink checks real partial-write behavior rather than a replacement mixer.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def production_functions():
    source = (HERE / "xiaozhi_client.c").read_text()

    def section(start, end):
        begin = source.index(start)
        return source[begin:source.index(end, begin)]

    return "\n".join((
        section("static void ui_feedback_changed(const struct panel_control *old", "\n#ifndef XIAOZHI_TEST_NO_ALSA\nstatic void ui_feedback_overlay"),
        section("static void ui_feedback_overlay(int32_t *stereo", "\n#endif\n/* glibc"),
        section("static int play_samples(struct audio *a", "\nstatic int drain_playback"),
        section("static int drain_playback(struct audio *a", "\n#endif\nstatic int decode_audio"),
    ))


PREFIX = r'''
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include "ui_feedback_pcm.h"
#define DECODE_FRAMES 5760
#define S12_STATE_XRUN 4
#define S12_STATE_SUSPENDED 7
#define S12_STATE_DISCONNECTED 8
#define S12_STATE_SETUP 1
struct audio {void *pcm;int gain,ui_prompt;uint64_t samples;};
struct panel_control {uint32_t seq,gain,muted,cancel,manual;};
static struct panel_control panel;
static struct {
    unsigned initialized,pending,active,offset;
    int64_t first,updated,last;
} ui_feedback;
static int stopping,panel_interrupted,panel_enabled;
static int64_t clock_ms=1000;
static int64_t now_ms(void){return clock_ms;}
static int remaining(int64_t deadline){(void)deadline;return 10000;}
static void panel_poll(void){}
static int fail(const char *message){fprintf(stderr,"%s\n",message);return -1;}
static int open_playback(struct audio *a){a->pcm=(void *)1;return 0;}
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
static int32_t output[200000];
static unsigned used,writes,waits;
static snd_pcm_sframes_t snd_pcm_writei(void *pcm,const void *values,snd_pcm_uframes_t frames){
    (void)pcm;++writes;
    if(writes%7==0)return -EAGAIN;
    if(writes%11==0)return 0;
    if(frames>7)frames=7;
    assert(used+frames*2<sizeof(output)/sizeof(*output));
    memcpy(output+used,values,frames*2*sizeof(int32_t));used+=(unsigned)frames*2;
    return (snd_pcm_sframes_t)frames;
}
static int snd_pcm_wait(void *pcm,int timeout){(void)pcm;(void)timeout;++waits;return 0;}
static int snd_pcm_get_params(void *pcm,snd_pcm_uframes_t *buffer,snd_pcm_uframes_t *period){
    (void)pcm;*buffer=15360;*period=960;return 0;
}
static int snd_pcm_state(void *pcm){(void)pcm;return S12_STATE_SETUP;}
static int snd_pcm_drain(void *pcm){(void)pcm;return 0;}
'''

HARNESS = r'''
static void reset(void){
    memset(&ui_feedback,0,sizeof(ui_feedback));memset(&panel,0,sizeof(panel));
    stopping=panel_interrupted=panel_enabled=0;clock_ms=1000;used=writes=waits=0;
}
static void change(struct panel_control next){ui_feedback_changed(&panel,&next);panel=next;}
static unsigned prompt_frames(unsigned kind){
    return (unsigned)(kind==1?S12_VOLUME_PROMPT_SAMPLES:S12_BUTTON_PROMPT_SAMPLES)*3;
}
static int16_t sample_at(unsigned kind,unsigned frame){
    assert(frame<prompt_frames(kind));
    const uint8_t *pcm=kind==1?s12_volume_prompt_pcm:s12_button_prompt_pcm;
    unsigned at=frame/3*2;
    return (int16_t)((unsigned)pcm[at]|(unsigned)pcm[at+1]<<8);
}
static int32_t expected(int16_t mono,unsigned frame,int gain,unsigned kind){
    int64_t value=(int64_t)mono*gain*65536/1000;
    if(frame<prompt_frames(kind))value+=(int64_t)sample_at(kind,frame)*gain*65536/1000;
    return value>INT32_MAX?INT32_MAX:value<INT32_MIN?INT32_MIN:(int32_t)value;
}
static void stream(unsigned total,unsigned kind){
    reset();ui_feedback.pending=kind;ui_feedback.first=ui_feedback.updated=clock_ms-200;
    struct audio a={0};a.gain=100;unsigned at=0;
    while(at<total){
        unsigned count=total-at;if(count>3000)count=3000;
        int16_t samples[3000];for(unsigned i=0;i<count;++i)samples[i]=(int16_t)((int)((at+i)%1000)-500);
        assert(!play_samples(&a,samples,(int)count,9999));a.samples+=count;at+=count;
    }
    assert(!drain_playback(&a,9999));
    unsigned frames=total<prompt_frames(kind)?(prompt_frames(kind)+959)/960*960:total;
    assert(used==frames*2&&waits>0&&writes>frames/7);
    for(unsigned i=0;i<frames;++i){
        int16_t mono=i<total?(int16_t)((int)(i%1000)-500):0;
        int32_t value=expected(mono,i,a.gain,kind);
        assert(output[i*2]==value&&output[i*2+1]==value);
    }
    assert(!ui_feedback.active&&a.samples==total);
    printf("exact_stream original_frames=%u sink_frames=%u writes=%u\n",total,frames,writes);
}
int main(int argc,char **argv){
    assert(argc==2);reset();
    if(!strcmp(argv[1],"streams")){
        stream(23040,1);stream(37,1);stream(23040,2);stream(37,2);
    }else if(!strcmp(argv[1],"queue")){
        change((struct panel_control){0,50,0,0,0});assert(!ui_feedback.pending);
        change((struct panel_control){1,50,0,0,0});assert(!ui_feedback.pending);
        change((struct panel_control){2,60,0,0,0});assert(ui_feedback.pending==1&&!ui_feedback_ready());
        clock_ms+=100;change((struct panel_control){3,70,0,0,0});assert(ui_feedback.pending==1&&!ui_feedback_ready());
        clock_ms+=159;assert(!ui_feedback_ready());++clock_ms;assert(ui_feedback_ready());
        change((struct panel_control){4,70,1,0,0});assert(ui_feedback.pending==2);
        change((struct panel_control){5,80,1,0,0});assert(ui_feedback.pending==2);
        change((struct panel_control){6,80,1,0,1});assert(!ui_feedback.pending);
        change((struct panel_control){7,90,1,0,1});ui_feedback.active=1;
        change((struct panel_control){8,0,1,0,1});assert(!ui_feedback.pending&&!ui_feedback.active);
    }else if(!strcmp(argv[1],"rate")){
        ui_feedback.initialized=1;panel.gain=50;
        for(unsigned i=0;i<5;++i){change((struct panel_control){i+1,51+i,0,0,0});if(i<4){assert(!ui_feedback_ready());clock_ms+=100;}}
        assert(ui_feedback_ready()); /* continuous changes cannot defer past400ms */
        ui_feedback.last=clock_ms;assert(!ui_feedback_ready());clock_ms+=200;assert(ui_feedback_ready());
    }else if(!strcmp(argv[1],"silent-prompt")){
        struct audio a={0};a.gain=100;a.ui_prompt=1;int16_t mono[37];for(unsigned i=0;i<37;++i)mono[i]=123;
        ui_feedback.pending=1;ui_feedback.first=ui_feedback.updated=clock_ms-200;
        assert(!play_samples(&a,mono,37,9999));assert(ui_feedback.pending==1&&!ui_feedback.active&&used==74);
        for(unsigned i=0;i<used;++i)assert(output[i]==(int32_t)((int64_t)123*100*65536/1000));
        a.ui_prompt=0;a.gain=0;ui_feedback.active=1;used=0;
        assert(!play_samples(&a,mono,37,9999));assert(!ui_feedback.pending&&!ui_feedback.active);
        for(unsigned i=0;i<used;++i)assert(output[i]==0);
    }else if(!strcmp(argv[1],"saturation")){
        for(unsigned sign=0;sign<2;++sign){
            unsigned frame=0;while(frame<prompt_frames(1)&&(sign?sample_at(1,frame)>=0:sample_at(1,frame)<=0))++frame;assert(frame<prompt_frames(1));
            ui_feedback.active=1;ui_feedback.offset=frame;int32_t stereo[2]={sign?INT32_MIN+1:INT32_MAX-1,sign?INT32_MIN+1:INT32_MAX-1};
            ui_feedback_overlay(stereo,1,100);assert(stereo[0]==(sign?INT32_MIN:INT32_MAX)&&stereo[1]==stereo[0]);
        }
    }else assert(!"unknown case");
    return 0;
}
'''


class UiFeedbackRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.directory = tempfile.TemporaryDirectory(prefix="s12-ui-feedback-")
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = directory / "sink.c"
        source.write_text(PREFIX + production_functions() + HARNESS)
        cls.binary = directory / "sink"
        subprocess.run([compiler, "-std=c11", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I", str(HERE), str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=10, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        return result.stdout

    def test_real_mixer_preserves_frames_under_short_writes_and_eagain(self):
        self.assertEqual(self.run_case("streams").count("exact_stream"), 4)

    def test_initial_duplicate_zero_manual_and_key_priority(self):
        self.run_case("queue")

    def test_sliding_coalesces_but_has_maximum_wait_and_rate_cap(self):
        self.run_case("rate")

    def test_prompt_owner_excludes_overlay_and_zero_gain_is_silent(self):
        self.run_case("silent-prompt")

    def test_overlay_saturates_both_channels(self):
        self.run_case("saturation")


if __name__ == "__main__":
    unittest.main()
