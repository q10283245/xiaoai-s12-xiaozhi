#!/usr/bin/env python3
"""Bounded multi-turn/keepalive: synthetic RAM audio and localhost mocks only."""
import json
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import subprocess
import tempfile
import time
import threading
import unittest
from test_xiaozhi_client import ClientTest, frame
HERE = Path(__file__).resolve().parent
HARNESS = r'''
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <sys/socket.h>
#include <errno.h>
#include <stddef.h>
static unsigned send_calls,send_depth,max_send_depth;
static size_t send_limit;
static int send_alternate,send_cancel_after_header;
static ssize_t intercepted_send(int fd,const void *data,size_t n,int flags);
#define send intercepted_send
#define XIAOZHI_TEST_NO_ALSA
#define XIAOZHI_TEST_FAKE_CAPTURE
#define main client_program_main
#include "vendor_wake_test.c"
#include "xiaozhi_client.c"
#undef main
#undef send
#include <assert.h>
static ssize_t intercepted_send(int fd,const void *data,size_t n,int flags) {
    ++send_calls;++send_depth;if(send_depth>max_send_depth)max_send_depth=send_depth;
    ssize_t r;
    if(send_alternate&&!(send_calls%2)){errno=EAGAIN;r=-1;}
    else {if(send_limit&&n>send_limit)n=send_limit;r=send(fd,data,n,flags);}
    if(send_cancel_after_header&&r>0){panel_interrupted=1;send_cancel_after_header=0;}
    --send_depth;return r;
}
static void read_exact(int fd,void *out,size_t n) {
    uint8_t *p=out;while(n){ssize_t r=recv(fd,p,n,0);assert(r>0);p+=r;n-=(size_t)r;}
}
static void read_frame(int fd,unsigned opcode,const char *body,uint8_t mask[4]) {
    uint8_t h[2];read_exact(fd,h,2);assert(h[0]==(0x80|opcode)&&(h[1]&0x80));
    size_t n=h[1]&127;if(n==126){uint8_t b[2];read_exact(fd,b,2);n=(size_t)b[0]*256+b[1];}
    assert(n<1024);read_exact(fd,mask,4);char data[1024];read_exact(fd,data,n);
    for(size_t i=0;i<n;++i)data[i]^=mask[i%4];data[n]=0;assert(!strcmp(data,body));
}
int main(int argc,char **argv) {
    assert(argc==2);int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));
    struct wire w={0};w.fd=fds[0];w.deadline=now_ms()+3000;signal(SIGPIPE,SIG_IGN);
    if(!strcmp(argv[1],"frame-order")) {
        assert(!session_keepalive_bind(&w,"mock-session"));session_keepalive.next=now_ms();
        char keepalive[1024];memcpy(keepalive,session_keepalive.payload,session_keepalive.payload_bytes);keepalive[session_keepalive.payload_bytes]=0;
        send_limit=1;send_alternate=1;session_keepalive_poll();assert(session_keepalive.bytes&&session_keepalive.sent==1);
        assert(!send_frame(&w,1,(const uint8_t *)"ordinary-frame",14));
        uint8_t first[4],second[4],ordinary[4];read_frame(fds[1],9,keepalive,first);read_frame(fds[1],1,"ordinary-frame",ordinary);
        session_keepalive.next=now_ms();int64_t end=now_ms()+1000;
        do {session_keepalive_poll();}while(session_keepalive.bytes&&remaining(end));
        assert(!session_keepalive.bytes&&!session_keepalive.failed);read_frame(fds[1],9,keepalive,second);
        assert(memcmp(first,second,4));assert(max_send_depth==1&&!w.writing);
    } else if(!strcmp(argv[1],"partial-timeout")) {
        assert(!session_keepalive_bind(&w,"mock-session"));send_limit=1;session_keepalive.next=now_ms();session_keepalive_poll();
        assert(session_keepalive.sent==1);session_keepalive.deadline=now_ms()-1;session_keepalive_poll();assert(session_keepalive.failed);
        unsigned calls=send_calls;assert(send_frame(&w,1,(const uint8_t *)"must-not-send",13)<0&&send_calls==calls);
        session_wire_close(&w,1);assert(!session_keepalive.wire&&w.fd==-1&&send_calls==calls);
        uint8_t byte;assert(recv(fds[1],&byte,1,0)==1);assert(!recv(fds[1],&byte,1,0));
    } else if(!strcmp(argv[1],"normal-frame-cancel")) {
        assert(!session_keepalive_bind(&w,"mock-session"));send_cancel_after_header=1;
        assert(send_frame(&w,1,(const uint8_t *)"payload-not-written",19)<0&&w.incomplete);
        panel_interrupted=0;unsigned calls=send_calls;
        assert(send_frame(&w,1,(const uint8_t *)"abort",5)<0&&send_calls==calls);
        session_keepalive.next=now_ms();session_keepalive_poll();assert(session_keepalive.failed&&send_calls==calls);
        session_wire_close(&w,1);assert(!session_keepalive.wire&&send_calls==calls);
        uint8_t bytes[16];ssize_t n=recv(fds[1],bytes,sizeof(bytes),0);assert(n==6);assert(!recv(fds[1],bytes,sizeof(bytes),0));
    } else if(!strcmp(argv[1],"followup-bounds")) {
        followup_seconds=8;int64_t deadline=now_ms()+120000;
        assert(conversation_continue(1,deadline,1,0,1));assert(!conversation_continue(8,deadline,1,0,1));
        assert(!conversation_continue(1,now_ms()-1,1,0,1));assert(!conversation_continue(1,deadline,1,0,0));
        assert(!conversation_continue(1,deadline,2,2,1));
        panel_enabled=1;panel.muted=1;assert(!conversation_continue(1,deadline,1,0,1));panel.muted=0;
        panel_interrupted=1;assert(!conversation_continue(1,deadline,1,0,1));panel_interrupted=0;
        stopping=1;assert(!conversation_continue(1,deadline,1,0,1));stopping=0;
        followup_seconds=0;assert(!conversation_continue(1,deadline,1,0,1));
    } else if(!strcmp(argv[1],"peer-close")) {
        assert(!session_keepalive_bind(&w,"mock-session"));followup_capture_deadline=now_ms()+120000;
        close(fds[1]);fds[1]=-1;session_keepalive_poll();assert(session_keepalive.failed&&!strcmp(barge_error_reason,"peer_closed"));
        session_wire_close(&w,1);assert(!session_keepalive.wire&&w.fd==-1);
    } else if(!strcmp(argv[1],"reason-map")) {
        fail("ALSA playback XRUN");assert(!strcmp(barge_failure,"playback")&&!strcmp(barge_error_reason,"playback_xrun"));
        fail("invalid server JSON");assert(!strcmp(barge_failure,"protocol")&&!strcmp(barge_error_reason,"protocol"));
        fail("capture deadline expired with incomplete audio");assert(!strcmp(barge_failure,"capture")&&!strcmp(barge_error_reason,"timeout"));
        fail("socket peer closed");assert(!strcmp(barge_failure,"network")&&!strcmp(barge_error_reason,"peer_closed"));
    } else assert(!"unknown case");
    if(w.fd>=0)session_wire_close(&w,0);if(fds[1]>=0)close(fds[1]);return 0;
}
'''

class FollowupTest(unittest.TestCase):
    tearDownClass = classmethod(ClientTest.tearDownClass.__func__)
    invoke = ClientTest.invoke
    wake_env = staticmethod(ClientTest.wake_env)
    @classmethod
    def setUpClass(cls):
        ClientTest.setUpClass.__func__(cls)
        source=cls.root/'followup-harness.c';source.write_text(HARNESS);cls.harness=cls.root/'followup-harness'
        subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(HERE),str(source),
                        *native_lib_flags(),'-o',str(cls.harness)],check=True)
    def env(self, **changes):
        result=dict(self.wake_env('wake-array'),S12_TEST_LOCAL_WAKE_FRAME='10');result.update(changes);return result
    def receive(self,p):
        while True:
            op,data=p.receive()
            if op==1:
                msg=json.loads(data)
                if msg.get('type')=='listen' and msg.get('state')=='keepalive':
                    self.assertEqual(msg['session_id'],'mock-session');continue
            return op,data
    def upload(self,p):
        op,data=self.receive(p);self.assertEqual(op,1)
        start=json.loads(data);turn=start.pop('turn_id');self.assertRegex(turn,r'^[0-9a-f]{32}$');p.turn_id=turn
        self.assertEqual(start,dict(session_id='mock-session',type='listen',state='start',mode='manual',source='manual'))
        packets=[]
        while True:
            op,data=self.receive(p)
            if op==1:
                stop=json.loads(data);self.assertEqual(stop.pop('turn_id'),turn)
                diagnostics=stop.pop('capture_stats',None)
                if diagnostics is not None:
                    self.assertEqual(set(diagnostics),{'reason','packets','duration_ms','speech_frames','vad_frames','max_silence_ms','noise_rms','threshold_rms','min_rms','max_rms'})
                    self.assertEqual(diagnostics['packets'],len(packets));self.assertEqual(diagnostics['duration_ms'],len(packets)*60)
                    self.assertLessEqual(diagnostics['speech_frames'],diagnostics['vad_frames']);self.assertLessEqual(diagnostics['vad_frames'],len(packets)*6)
                    p.capture_stats=diagnostics
                self.assertEqual(stop,dict(session_id='mock-session',type='listen',state='stop'))
                self.assertGreater(len(packets),0);return packets
            self.assertEqual(op,2);packets.append(data)
    def answer(self,p,music=False):
        content=frame(1,dict(session_id='mock-session',type='tts',state='start'))
        if music:content+=frame(1,dict(session_id='mock-session',type='music',state='playing'))
        p.sock.sendall(content+frame(2,self.packet)+frame(1,dict(session_id='mock-session',type='tts',state='stop')))
    def test_two_rounds_same_session_manual_no_second_wake_cue(self):
        uploads=[]
        def server(p):
            p.normal_hello()
            for _ in range(2):uploads.append(self.upload(p));self.answer(p)
            self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','2'],env_change=self.env())
        self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(len(uploads),2);self.assertEqual([e['prompt'] for e in events if e['event']=='local_prompt'],['wake','followup'])
        self.assertEqual(sum(e['event']=='followup_listening' for e in events),1)
        self.assertEqual(sum(e['event']=='hello' for e in events),1)
    def test_music_then_reply_fresh_audio_state_and_deadline(self):
        def server(p):
            p.normal_hello();self.upload(p);self.answer(p,music=True)
            self.upload(p);time.sleep(.05);self.answer(p);self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','2'],env_change=self.env())
        self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(sum(e['event']=='music_stream_start' for e in events),1)
        complete=[e for e in events if e['event']=='turn_complete']
        self.assertEqual([e['opus_packets'] for e in complete],[1,1])
        self.assertEqual([e['decoded_frames_48000'] for e in complete],[2880,2880])
        self.assertEqual([e['buffered'] for e in complete],[False,True])
    def test_silent_followup_no_upload_or_second_cue(self):
        def server(p):
            p.normal_hello();self.upload(p);self.answer(p);self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','1'],env_change=self.env(S12_TEST_CAPTURE_SILENT_FROM_GENERATION='2'),interrupt_after=.6)
        self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(sum(e['event']=='utterance_uploaded' for e in events),1)
        self.assertEqual(sum(e['event']=='followup_listening' for e in events),1)
        self.assertEqual([e['prompt'] for e in events if e['event']=='local_prompt'],['wake','followup'])
        self.assertIn('local_command_timeout',[e['event'] for e in events])
    def test_peer_close_followup_recovers_no_stale_reconnect(self):
        def server(p):
            p.normal_hello();self.upload(p);self.answer(p);time.sleep(.03)
        r=self.invoke(server,['--local-wake','--followup-seconds','8'],env_change=self.env(S12_TEST_CAPTURE_FAST='0',S12_TEST_CAPTURE_SILENT_FROM_GENERATION='2'),interrupt_after=3)
        self.assertEqual(r.returncode,0,r.stderr);self.assertIn(b'socket peer closed during followup',r.stderr)
        events=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(sum(e['event']=='utterance_uploaded' for e in events),1)
        self.assertEqual(sum(e['event']=='hello' for e in events),1)
        self.assertGreaterEqual(sum(e['event']=='local_wake_listening' for e in events),2)
    def test_followup_mute_and_ota_close_session_before_pause(self):
        for mode in ('mute', 'ota'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=self.root) as td:
                directory=Path(td);control=directory/'panel.control';control.write_text('v1 0 50 0 0 0\n');control.chmod(0o600)
                ota=directory/'ota';ota.mkdir(mode=0o700)
                finished=threading.Event()
                def server(p):
                    p.normal_hello();self.upload(p);self.answer(p)
                    until=time.monotonic()+1
                    while time.monotonic()<until:
                        state=directory/'ui.state'
                        if state.exists() and state.read_text()=='capturing\n':break
                        time.sleep(.002)
                    else:self.fail('no follow-up capture state')
                    if mode=='mute':
                        new=directory/'new';new.write_text('v1 1 50 1 0 0\n');new.chmod(0o600);new.replace(control)
                    else:
                        request=ota/'auto-apply.request';request.write_text('a'*48+'\n');request.chmod(0o600)
                    # No second listen.start/binary/stop is authorized on cancel.
                    try:op,_=self.receive(p);self.assertEqual(op,8)
                    except EOFError:pass
                    if mode=='ota':
                        until=time.monotonic()+3
                        while time.monotonic()<until and not (ota/'auto-apply.idle').exists():time.sleep(.005)
                        self.assertTrue((ota/'auto-apply.idle').exists())
                    finished.set()
                env=self.env(S12_TEST_CAPTURE_FAST='0',S12_TEST_CAPTURE_SILENT_FROM_GENERATION='2',S12_TEST_PANEL_DIR=td,S12_TEST_OTA_DIR=str(ota))
                r=self.invoke(server,['--local-wake','--followup-seconds','8','--panel'],env_change=env,interrupt_after=10,stop_event=finished)
                self.assertTrue(finished.is_set(),'mock did not reach bounded post-capture control acknowledgment')
                self.assertEqual(r.returncode,0,r.stderr);events=[json.loads(x) for x in r.stdout.splitlines()]
                self.assertEqual(sum(e['event']=='utterance_uploaded' for e in events),1)
                prompts=[e['prompt'] for e in events if e['event']=='local_prompt']
                self.assertEqual(prompts[:2],['wake','followup'])
                self.assertIn(prompts[2:],[[],['button']] if mode=='mute' else [[]])
                if mode=='ota':self.assertIn('ota_idle_enter',[e['event'] for e in events])

    def test_keepalive_while_waiting_server_reply(self):
        keepalives=[]
        def server(p):
            p.normal_hello();self.upload(p)
            for _ in range(3):
                op,data=p.receive(include_ping=True);self.assertEqual(op,9);self.assertEqual(data,b's12-ping');keepalives.append(data);p.sock.sendall(frame(10,data))
            self.answer(p);self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','1'],env_change=self.env(S12_TEST_KEEPALIVE_MS='20'))
        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(len(keepalives),3)
    def test_unknown_empty_response_no_followup(self):
        def server(p):
            p.normal_hello();self.upload(p);p.sock.sendall(frame(1,dict(type='tts',state='stop',session_id='mock-session')));self.assertEqual(self.receive(p)[0],8)
        r=self.invoke(server,['--local-wake','--followup-seconds','8','--wake-rounds','1'],env_change=self.env())
        self.assertEqual(r.returncode,0,r.stderr);self.assertNotIn(b'followup_listening',r.stdout)
    def test_native_frame_boundaries_limits_and_reason_mapping(self):
        for case in ('frame-order','partial-timeout','normal-frame-cancel','followup-bounds','peer-close','reason-map'):
            with self.subTest(case=case):
                r=subprocess.run([str(self.harness),case],capture_output=True,timeout=4)
                self.assertEqual(r.returncode,0,r.stdout+r.stderr)
    def test_cli_requires_local_wake_and_bounded_seconds(self):
        for args in (['--wake-loop','--followup-seconds','8'],['--local-wake','--followup-seconds','9']):
            self.assertEqual(self.invoke(args=args).returncode,2)
if __name__=='__main__':unittest.main()
