#!/usr/bin/env python3
"""Semantic follow-up binding/cue gates, synthetic RAM audio/localhost only."""
import json
import os
import signal
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags
import subprocess
import tempfile
import threading
import time
import unittest
import test_xiaozhi_client as base
import test_xiaozhi_followup as follow
HERE=Path(__file__).resolve().parent
HARNESS=r'''
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define XIAOZHI_TEST_NO_ALSA
#define XIAOZHI_TEST_FAKE_CAPTURE
#define main client_program_main
#include "vendor_wake_test.c"
#include "xiaozhi_client.c"
#undef main
#include <assert.h>
#define META(state) "{\"type\":\"dialogue\",\"session_id\":\"mock-session\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"state\":" state "}"
static void init(struct wire *w) {memset(w,0,sizeof(*w));w->fd=-1;strcpy(w->turn_id,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");followup_seconds=8;}
static void meta(struct wire *w,const char *raw,int started,int audio) {
    struct json_object *j=parse_json((const uint8_t *)raw,strlen(raw));assert(j);
    dialogue_receive(w,j,"mock-session",(const uint8_t *)raw,strlen(raw),started,audio);json_object_put(j);
}
static void *release_ota(void *opaque) {
    const char *directory=opaque;char idle[4096],request[4096];
    snprintf(idle,sizeof(idle),"%s/auto-apply.idle",directory);snprintf(request,sizeof(request),"%s/auto-apply.request",directory);
    int64_t end=now_ms()+1000;while(access(idle,F_OK)&&remaining(end)){struct timespec delay={0,2000000};nanosleep(&delay,NULL);}
    assert(!access(idle,F_OK));assert(!unlink(request));return NULL;
}
static void *manual_during_cue(void *opaque) {
    const char *directory=opaque;char path[4096],next[4096];
    snprintf(path,sizeof(path),"%s/panel.control",directory);snprintf(next,sizeof(next),"%s/panel.next",directory);
    const char *test_delay=getenv("S12_TEST_MANUAL_DELAY_MS");struct timespec delay={0,(test_delay?strtol(test_delay,NULL,10):50)*1000000};nanosleep(&delay,NULL);
    int fd=open(next,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);const char value[]="v1 1 50 0 0 1\n";
    assert(write(fd,value,sizeof(value)-1)==sizeof(value)-1);close(fd);assert(!rename(next,path));return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);struct wire w;init(&w);
    if(!strcmp(argv[1],"states")) {
        assert(dialogue_wait_ms(&w)==8000);meta(&w,META("\"await_reply\""),1,1);assert(dialogue_wait_ms(&w)==20000);
        init(&w);meta(&w,META("\"allow_followup\""),1,1);assert(dialogue_wait_ms(&w)==8000);
        init(&w);meta(&w,META("\"close\""),0,0);assert(dialogue_wait_ms(&w)==0);
        followup_seconds=0;assert(dialogue_wait_ms(&w)==0);dialogue_reset(&w);assert(!w.turn_id[0]&&!w.dialogue_seen);
    } else if(!strcmp(argv[1],"stale")) {
        const char *messages[]={
            "{\"type\":\"dialogue\",\"state\":\"close\",\"session_id\":\"wrong\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
            "{\"type\":\"dialogue\",\"state\":\"close\",\"session_id\":\"mock-session\",\"turn_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}",
            "{\"type\":\"dialogue\",\"state\":\"close\",\"session_id\":44,\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
            "{\"type\":\"dialogue\",\"state\":\"close\",\"session_id\":\"mock-session\",\"turn_id\":null}"};
        meta(&w,META("\"await_reply\""),1,1);
        for(unsigned i=0;i<sizeof(messages)/sizeof(*messages);++i){meta(&w,messages[i],1,1);assert(!w.dialogue_invalid&&w.dialogue_seen==1&&dialogue_wait_ms(&w)==20000);}
    } else if(!strcmp(argv[1],"invalid")) {
        const char *messages[]={META("true"),META("null"),META("{}"),META("[]"),META("\"unknown\""),META("\"await_reply\\u0000junk\""),
            "{\"type\":\"dialogue\",\"session_id\":\"mock-session\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
            "{\"type\":\"dialogue\",\"session_id\":\"mock-session\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"state\":\"close\",\"state\":\"await_reply\"}",
            "{\"type\":\"dialogue\",\"session_id\":\"mock-session\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"state\":\"close\",\"duration\":999999}",
            "{\"type\":\"dialogue\",\"session_id\":\"mock-session\",\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"st\\u0061te\":\"close\"}"};
        for(unsigned i=0;i<sizeof(messages)/sizeof(*messages);++i){init(&w);meta(&w,messages[i],1,1);assert(w.dialogue_invalid&&dialogue_wait_ms(&w)==8000);}
        init(&w);meta(&w,META("\"await_reply\""),0,1);assert(w.dialogue_invalid);
        init(&w);meta(&w,META("\"await_reply\""),1,0);assert(w.dialogue_invalid);
    } else if(!strcmp(argv[1],"duplicate")) {
        meta(&w,META("\"await_reply\""),1,1);meta(&w,META("\"close\""),1,1);assert(w.dialogue_invalid&&dialogue_wait_ms(&w)==8000);
        init(&w);meta(&w,META("\"close\""),1,1);meta(&w,META("\"close\""),1,1);assert(w.dialogue_invalid&&dialogue_wait_ms(&w)==8000);
    } else if(!strcmp(argv[1],"new-request")) {
        meta(&w,META("\"close\""),0,0);assert(!dialogue_begin(&w)&&strlen(w.turn_id)==32&&!w.dialogue_seen&&!w.dialogue_invalid&&dialogue_wait_ms(&w)==8000);
        for(unsigned i=0;i<32;++i)assert((w.turn_id[i]>='a'&&w.turn_id[i]<='f')||(w.turn_id[i]>='0'&&w.turn_id[i]<='9'));
        char first[33];strcpy(first,w.turn_id);assert(!dialogue_begin(&w)&&strcmp(first,w.turn_id));
        session_wire_close(&w,0);assert(!w.turn_id[0]&&!w.dialogue_seen&&!w.dialogue_invalid);
        followup_seconds=0;assert(!dialogue_begin(&w)&&!w.turn_id[0]);
    } else if(!strcmp(argv[1],"cue-gates")) {
        int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));w.fd=fds[0];
        struct audio options={0};struct barge_monitor monitor={0};int64_t end=now_ms()+120000;
        assert(!followup_open(&w,&options,&monitor,now_ms()-1));assert(!followup_open(&w,&options,&monitor,now_ms()+1500));
        stopping=1;assert(!followup_open(&w,&options,&monitor,end));stopping=0;
        panel_interrupted=1;assert(!followup_open(&w,&options,&monitor,end));panel_interrupted=0;
        panel_enabled=1;panel.muted=1;assert(!followup_open(&w,&options,&monitor,end));panel.muted=0;panel_enabled=0;
        w.incomplete=1;assert(!followup_open(&w,&options,&monitor,end));w.incomplete=0;
        session_keepalive.failed=1;assert(!followup_open(&w,&options,&monitor,end));session_keepalive.failed=0;
        monitor.opened=1;assert(!followup_open(&w,&options,&monitor,end));monitor.opened=0;
        monitor.worker_started=1;assert(!followup_open(&w,&options,&monitor,end));monitor.worker_started=0;
        monitor.pending=1;assert(!followup_open(&w,&options,&monitor,end));monitor.pending=0;
        meta(&w,META("\"close\""),0,0);assert(!followup_open(&w,&options,&monitor,end));
        dialogue_reset(&w);assert(followup_open(&w,&options,&monitor,end)==1);close(fds[0]);close(fds[1]);
    } else if(!strcmp(argv[1],"ota-before-ui")) {
        char directory[]="/tmp/s12-ui-ota-XXXXXX";assert(mkdtemp(directory));assert(!chmod(directory,0700));
        assert(!setenv("S12_TEST_OTA_DIR",directory,1));char request[4096];snprintf(request,sizeof(request),"%s/auto-apply.request",directory);
        int fd=open(request,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);char nonce[49];memset(nonce,'a',48);nonce[48]='\n';assert(write(fd,nonce,49)==49);close(fd);
        setenv("S12_TEST_CAPTURE_PROFILE","wake-silence",1);setenv("S12_TEST_CAPTURE_FAST","1",1);
        struct vad v;vad_init(&v,250);struct mic_frontend frontend;mic_frontend_init(&frontend,0);
        struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(TTS_BUFFER_PACKETS,sizeof(*q.slices));assert(q.bytes&&q.slices);
        ui_feedback.pending=1;ui_feedback.first=ui_feedback.updated=now_ms()-200;followup_capture_deadline=now_ms()+5000;
        pthread_t releaser;assert(!pthread_create(&releaser,NULL,release_ota,directory));
        assert(capture_utterance(&v,&frontend,&q,NULL,20000,NULL)==2);assert(!pthread_join(releaser,NULL));
        assert(ui_feedback.pending==1&&!q.count&&!ota_idle_requested());assert(!rmdir(directory));
        followup_capture_deadline=followup_onset_deadline=0;free(q.bytes);free(q.slices);
    } else if(!strcmp(argv[1],"manual-before-input")) {
        struct vad v;vad_init(&v,250);struct mic_frontend frontend;mic_frontend_init(&frontend,0);struct tts_queue q={0};
        panel_enabled=1;panel.manual=1;panel_manual_used=0;
        assert(capture_utterance(&v,&frontend,&q,NULL,20000,NULL)==2&&!q.count);
        struct audio options={0};struct barge_monitor monitor={0};int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));w.fd=fds[0];
        assert(!followup_open(&w,&options,&monitor,now_ms()+20000));close(fds[0]);close(fds[1]);
    } else if(!strcmp(argv[1],"manual-during-cue")) {
        char directory[]="/tmp/s12-ui-manual-XXXXXX";assert(mkdtemp(directory));assert(!chmod(directory,0700));
        assert(!setenv("S12_TEST_PANEL_DIR",directory,1));assert(!setenv("S12_TEST_UI_PROMPT_MS","300",1));
        setenv("S12_TEST_CAPTURE_PROFILE","wake-silence",1);setenv("S12_TEST_CAPTURE_FAST","0",1);setenv("S12_TEST_MANUAL_DELAY_MS","230",1);
        char path[4096];snprintf(path,sizeof(path),"%s/panel.control",directory);int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
        const char value[]="v1 0 50 0 0 0\n";assert(write(fd,value,sizeof(value)-1)==sizeof(value)-1);close(fd);
        panel_enabled=1;panel.manual=panel_manual_used=0;int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));w.fd=fds[0];
        struct audio options={0};struct barge_monitor monitor={0};pthread_t gesture;
        assert(!pthread_create(&gesture,NULL,manual_during_cue,directory));
        assert(followup_open(&w,&options,&monitor,now_ms()+20000)==1);
        struct vad v;vad_init(&v,250);struct mic_frontend frontend;mic_frontend_init(&frontend,0);struct tts_queue q={0};
        q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(TTS_BUFFER_PACKETS,sizeof(*q.slices));assert(q.bytes&&q.slices);
        assert(capture_utterance(&v,&frontend,&q,NULL,20000,&options)==2&&!q.count);assert(!pthread_join(gesture,NULL));
        assert(panel.manual==1&&!panel_manual_used&&!monitor.opened&&!monitor.worker_started);free(q.bytes);free(q.slices);
        close(fds[0]);close(fds[1]);assert(!unlink(path));snprintf(path,sizeof(path),"%s/ui.state",directory);unlink(path);assert(!rmdir(directory));
    } else if(!strcmp(argv[1],"window-deadline")) {
        setenv("S12_TEST_CAPTURE_PROFILE","wake-silence",1);setenv("S12_TEST_CAPTURE_FAST","1",1);
        struct vad v;vad_init(&v,250);struct mic_frontend frontend;mic_frontend_init(&frontend,0);
        struct tts_queue q={0};q.bytes=malloc(TTS_BUFFER_BYTES);q.slices=calloc(TTS_BUFFER_PACKETS,sizeof(*q.slices));assert(q.bytes&&q.slices);
        struct audio options={0};int64_t start=now_ms(),original=start+90;
        followup_capture_deadline=start+5000;followup_onset_deadline=original;
        ui_feedback.pending=1;ui_feedback.first=ui_feedback.updated=start-200;
        assert(capture_utterance(&v,&frontend,&q,NULL,20000,NULL)==LOCAL_UI_FEEDBACK);
        assert(followup_onset_deadline==original&&!q.count);assert(ui_feedback_play(&options)==1);
        struct timespec delay={0,50000000};nanosleep(&delay,NULL);
        assert(capture_utterance(&v,&frontend,&q,NULL,20000,NULL)==3);
        assert(followup_onset_deadline==original&&!q.count&&now_ms()-start<250);
        followup_capture_deadline=followup_onset_deadline=0;free(q.bytes);free(q.slices);
    } else assert(!"unknown case");return 0;
}
'''
class DialogueTest(unittest.TestCase):
    tearDownClass=classmethod(base.ClientTest.tearDownClass.__func__)
    invoke=base.ClientTest.invoke
    wake_env=staticmethod(base.ClientTest.wake_env)
    env=follow.FollowupTest.env
    receive=follow.FollowupTest.receive
    upload=follow.FollowupTest.upload
    @classmethod
    def setUpClass(cls):
        base.ClientTest.setUpClass.__func__(cls)
        source=cls.root/'dialogue.c';source.write_text(HARNESS);cls.harness=cls.root/'dialogue'
        subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(HERE),str(source),*native_lib_flags(),'-o',str(cls.harness)],check=True)
    def test_native_states_types_stale_duplicates_unique_turn_and_cue_gates(self):
        for case in ['states','stale','invalid','duplicate','new-request','cue-gates','window-deadline','ota-before-ui','manual-before-input','manual-during-cue']:
            with self.subTest(case=case):
                r=subprocess.run([str(self.harness),case],capture_output=True,timeout=5,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
                self.assertEqual(r.returncode,0,r.stdout+r.stderr)
                if case=='cue-gates':
                    events=[json.loads(line) for line in r.stdout.splitlines()];self.assertEqual(sum(e['event']=='local_prompt' for e in events),0)
    def meta(self,p,state,**changes):
        msg=dict(type='dialogue',state=state,session_id='mock-session',turn_id=p.turn_id);msg.update(changes);return msg
    def answer(self,p,messages):
        content=base.frame(1,dict(session_id='mock-session',type='tts',state='start'))+base.frame(2,self.packet)
        for msg in messages:content+=base.frame(1,msg)
        p.sock.sendall(content+base.frame(1,dict(session_id='mock-session',type='tts',state='stop')))
    def roundtrip(self,metadata,window):
        ids=[]
        def server(p):
            p.normal_hello();self.upload(p);ids.append(p.turn_id);self.answer(p,metadata(p))
            self.upload(p);ids.append(p.turn_id);self.answer(p,[self.meta(p,'close')]);self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','2'],env_change=self.env())
        self.assertEqual(r.returncode,0,r.stderr);self.assertNotEqual(*ids)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual([e['prompt'] for e in events if e['event']=='local_prompt'],['wake','followup'])
        windows=[e for e in events if e['event']=='followup_listening'];self.assertEqual(len(windows),1)
        self.assertEqual(windows[0]['window_ms'],window);self.assertTrue(windows[0]['followup_cue_completed'])
        complete=next(i for i,e in enumerate(events) if e['event']=='turn_complete')
        cue=next(i for i,e in enumerate(events) if e['event']=='local_prompt' and e['prompt']=='followup')
        listen=next(i for i,e in enumerate(events[cue+1:],cue+1) if e['event']=='wake_listening')
        self.assertLess(complete,cue);self.assertGreater(listen,cue);return events
    def test_await_reply_opens_twenty_seconds(self):self.roundtrip(lambda p:[self.meta(p,'await_reply')],20000)
    def test_allow_and_missing_keep_eight_seconds(self):
        self.roundtrip(lambda p:[self.meta(p,'allow_followup')],8000);self.roundtrip(lambda p:[],8000)
    def test_stale_wrong_session_ignored_before_generic_fatal_check(self):
        events=self.roundtrip(lambda p:[self.meta(p,'close',session_id='old'),self.meta(p,'close',turn_id='b'*32),self.meta(p,'await_reply')],20000)
        self.assertEqual(sum(e['event']=='dialogue_ignored' for e in events),2)
    def test_invalid_or_duplicate_current_falls_back_without_protocol_failure(self):
        for metadata in [lambda p:[self.meta(p,True)],lambda p:[self.meta(p,'close',duration_ms=999999)],lambda p:[self.meta(p,'await_reply'),self.meta(p,'close')]]:
            with self.subTest(metadata=metadata):self.roundtrip(metadata,8000)
    def test_close_and_stop_only_never_followup_or_error_prompt(self):
        for audio in (True,False):
            with self.subTest(audio=audio):
                def server(p):
                    p.normal_hello();self.upload(p)
                    if audio:self.answer(p,[self.meta(p,'close')])
                    else:p.sock.sendall(base.frame(1,self.meta(p,'close'))+base.frame(1,dict(type='tts',state='stop',session_id='mock-session')))
                    self.assertEqual(self.receive(p)[0],8)
                r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','1'],env_change=self.env())
                self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(line) for line in r.stdout.splitlines()]
                self.assertEqual([e['prompt'] for e in events if e['event']=='local_prompt'],['wake'])
                self.assertFalse(any(e['event']=='followup_listening' for e in events))
                if not audio:self.assertIn('dialogue_closed',[e['event'] for e in events])
    def test_idle_ui_feedback_closes_capture_then_reopens_without_upload(self):
        for control_line,expected in [('v1 1 70 0 0 0\n','volume'),('v1 1 50 1 0 0\n','button')]:
            with self.subTest(expected=expected),tempfile.TemporaryDirectory(dir=self.root) as td:
                directory=Path(td);control=directory/'panel.control';control.write_text('v1 0 50 0 0 0\n');control.chmod(0o600)
                errors=[]
                def change():
                    deadline=time.monotonic()+2
                    while time.monotonic()<deadline:
                        state=directory/'local-wake.state'
                        if state.exists() and state.read_text()=='ready\n':break
                        time.sleep(.01)
                    else:errors.append('idle input never ready');return
                    time.sleep(.10)
                    new=directory/'new';new.write_text(control_line);new.chmod(0o600);new.replace(control)
                worker=threading.Thread(target=change);worker.start()
                env=self.env(S12_TEST_CAPTURE_FAST='0',S12_TEST_CAPTURE_SILENT_FROM_GENERATION='0',S12_TEST_PANEL_DIR=td,S12_TEST_LOCAL_WAKE_FRAME='999999')
                r=self.invoke(args=['--local-wake','--followup-seconds','8','--panel'],env_change=env,interrupt_after=1.6)
                worker.join(3);self.assertFalse(errors,errors);self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                cue=next(i for i,e in enumerate(events) if e['event']=='local_prompt' and e['prompt']==expected)
                closed=max(i for i,e in enumerate(events[:cue]) if e['event']=='local_capture_closed')
                self.assertLess(closed,cue);self.assertNotIn('utterance_uploaded',[e['event'] for e in events])
                self.assertNotIn('hello',[e['event'] for e in events])
                self.assertEqual(sum(e['event']=='local_prompt' for e in events),1)
                ui=next(e for e in events if e['event']=='ui_feedback');self.assertLessEqual(ui['queued_ms'],600)
                if expected=='volume':self.assertGreater(sum(e['event']=='local_wake_listening' for e in events),1)
                else:self.assertEqual((directory/'ui.state').read_text(),'muted\n')

    def test_manual_button_uses_one_key_tone_and_same_capture_window(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            directory=Path(td);control=directory/'panel.control';control.write_text('v1 0 60 0 0 0\n');control.chmod(0o600)
            def press():
                deadline=time.monotonic()+2
                while time.monotonic()<deadline:
                    state=directory/'local-wake.state'
                    if state.exists() and state.read_text()=='ready\n':break
                    time.sleep(.005)
                new=directory/'new';new.write_text('v1 1 60 0 0 1\n');new.chmod(0o600);new.replace(control)
            worker=threading.Thread(target=press);worker.start()
            def server(p):
                p.normal_hello();self.upload(p);self.answer(p,[self.meta(p,'close')]);self.assertEqual(self.receive(p)[0],8)
            r=self.invoke(server,['--local-wake','--followup-seconds','8','--panel','--wake-rounds','1'],env_change=self.env(S12_TEST_PANEL_DIR=td,S12_TEST_LOCAL_WAKE_FRAME='999999'))
            worker.join(3)
            self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(line) for line in r.stdout.splitlines()]
            self.assertEqual([e['prompt'] for e in events if e['event']=='local_prompt'],['button'])
            self.assertEqual(sum(e['event']=='utterance_uploaded' for e in events),1)

    def test_capture_complete_cue_mute_cancel_ota_never_reopen_barge_or_upload(self):
        for mode in ['mute','cancel','ota','manual']:
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(dir=self.root) as td:
                directory=Path(td);control=directory/'panel.control'
                control.write_text('v1 0 50 0 0 0\n');control.chmod(0o600)
                token=directory/'token';token.write_text(base.FAKE_TOKEN+'\n');token.chmod(0o600)
                cfg=directory/'config.json';cfg.write_text(json.dumps({'url':'ws://127.0.0.1:9/xiaozhi/v1/','deviceid':'02:00:00:00:00:01','clientid':'offline-client','token-file':str(token)}))
                validation=dict(schema=1,reference_verified=True,capture_rate=48000,capture_channels=8,ref_channel=6,ref_delay_samples=0,ref_gain_permille=1000,device='S12A',vendor_sha256='79f1a33d9683cd6940c8d23e3dd4e992f1958fe220c0dfff8d230f9f8a1b5e73',source_image='a'*64)
                barge=directory/'barge.json';barge.write_text(json.dumps(validation));barge.chmod(0o600)
                marker=directory/'marker.json';marker.write_text(json.dumps(validation));marker.chmod(0o600)
                ota=directory/'ota';ota.mkdir(mode=0o700)
                env=dict(os.environ,**self.env(S12_TEST_CAPTURE_FAST='0',S12_TEST_PANEL_DIR=td,S12_TEST_BARGE_MARKER=str(marker),S12_TEST_OTA_DIR=str(ota),S12_TEST_UI_PROMPT_MS='300'))
                command=[str(self.binary),'--config',str(cfg),'--timeout','2','--local-wake','--followup-seconds','8','--panel','--playback','--barge-in','--barge-config',str(barge)]
                process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
                deadline=threading.Timer(9,process.kill);deadline.start();events=[];changed=False;cancelled=False;discarded=False
                try:
                    for raw in iter(process.stdout.readline,b''):
                        event=json.loads(raw);events.append(event)
                        if event['event']=='speech_detected' and not changed:
                            changed=True;new=directory/'new';new.write_text('v1 1 80 0 0 0\n');new.chmod(0o600);new.replace(control)
                        if event['event']=='local_prompt' and event['prompt']=='volume':
                            cancelled=True
                            if mode=='ota':
                                request=ota/'auto-apply.request';request.write_text('a'*48+'\n');request.chmod(0o600)
                            else:
                                new=directory/'new';new.write_text('v1 2 80 '+('1 0 0' if mode=='mute' else '0 0 1' if mode=='manual' else '0 1 0')+'\n');new.chmod(0o600);new.replace(control)
                        if event['event']=='ui_feedback_turn_discarded':
                            discarded=True;process.send_signal(signal.SIGTERM);break
                    out,err=process.communicate(timeout=3)
                finally:
                    deadline.cancel()
                    if process.poll() is None:process.kill();process.wait()
                self.assertEqual(process.returncode,0,err);self.assertTrue(changed);self.assertTrue(cancelled);self.assertTrue(discarded,events)
                self.assertNotIn('hello',[e['event'] for e in events]);self.assertNotIn('utterance_uploaded',[e['event'] for e in events])
                status=json.loads((directory/'barge-status.json').read_text());self.assertEqual(status['monitor_starts'],0);self.assertEqual(status['schema'],3)
                self.assertFalse(next(e for e in events if e['event']=='ui_feedback_turn_discarded')['capture_reopened'])

if __name__=='__main__':unittest.main()
