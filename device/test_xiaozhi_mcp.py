#!/usr/bin/env python3
"""MCP volume/status: localhost WS and private fake-panel IPC only; no devices."""
import json
import os
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from test_xiaozhi_client import ClientTest,Mock,frame
from test_panel_io import Fixture
HERE=Path(__file__).resolve().parent

def send_rpc(p,ident,method,params=None,**extra):
    payload=dict(jsonrpc='2.0',method=method,params={} if params is None else params)
    if ident is not ...:payload['id']=ident
    payload.update(extra);p.sock.sendall(frame(1,dict(type='mcp',payload=payload)))

def receive_rpc(p,ident=...):
    opcode,data=p.receive()
    assert opcode==1
    outer=json.loads(data)
    assert outer['type']=='mcp'
    rpc=outer['payload']
    assert rpc['jsonrpc']=='2.0'
    if ident is not ...:assert rpc['id']==ident,(rpc,ident)
    return rpc

def initialize(p):
    send_rpc(p,1,'initialize',dict(protocolVersion='2024-11-05',capabilities={},clientInfo=dict(name='offline',version='1')))
    result=receive_rpc(p,1)['result'];assert result['protocolVersion']=='2024-11-05'
    send_rpc(p,2,'tools/list');tools=receive_rpc(p,2)['result']['tools']
    assert {x['name'] for x in tools}=={'self.get_device_status','self.audio_speaker.set_volume'}
    volume=next(x for x in tools if x['name'].endswith('set_volume'))['inputSchema']
    assert volume==dict(type='object',properties={'volume':dict(type='integer',minimum=0,maximum=100)},required=['volume'],additionalProperties=False)

def call(p,ident,name,args):send_rpc(p,ident,'tools/call',dict(name=name,arguments=args))
def text_result(rpc):return json.loads(rpc['result']['content'][0]['text'])

class FakePanel:
    def __init__(self,root,muted=1,delay=0,fail=False,lost_ack=False):
        self.root=Path(root);self.delay=delay;self.fail=fail;self.lost_ack=lost_ack;self.requests=[];self.stop=threading.Event();self.error=None
        self.write('panel.control',f'v1 10 50 {muted} 7 11\n')
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def write(self,name,text):
        tmp=self.root/(name+'.fixture-new');tmp.write_text(text);tmp.chmod(0o600);tmp.replace(self.root/name)
    def run(self):
        try:
            while not self.stop.is_set():
                request=self.root/'panel-volume.request'
                if not request.exists():time.sleep(.002);continue
                claim=self.root/'.panel-volume.claim.fixture'
                try:request.replace(claim)
                except FileNotFoundError:continue
                fields=claim.read_text().split();assert len(fields)==4 and fields[0]=='v1'
                _,nonce,volume,deadline=fields;volume=int(volume);deadline=int(deadline)
                assert len(nonce)==32 and all(c in '0123456789abcdef' for c in nonce)
                self.requests.append((nonce,volume,deadline));time.sleep(self.delay)
                now=int(time.clock_gettime(time.CLOCK_MONOTONIC)*1000)
                if deadline<=now:ack=f'v1 {nonce} failed expired\n'
                elif self.fail:ack=f'v1 {nonce} failed publish_failed\n'
                else:
                    control=(self.root/'panel.control').read_text().split();seq=int(control[1])+1
                    self.write('panel.control',f'v1 {seq} {volume*2} {control[3]} {control[4]} {control[5]}\n')
                    ack=f'v1 {nonce} ok {volume} {volume*2} {seq}\n'
                if not self.lost_ack:self.write('panel-volume.ack',ack)
                claim.unlink()
        except Exception as ex:self.error=ex
    def close(self):
        self.stop.set();self.thread.join(3)
        if self.error:raise self.error

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
static void file(const char *name,const char *text) {char path[4096];assert(!panel_path(path,sizeof(path),name));int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));assert(!close(fd));}
static struct json_object *request(int id,int volume) {char raw[256];snprintf(raw,sizeof(raw),"{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{\"name\":\"self.audio_speaker.set_volume\",\"arguments\":{\"volume\":%d}}}}",id,volume);return parse_json(raw,strlen(raw));}
static struct json_object *receive_reply(int fd) {uint8_t h[2],mask[4],raw[2048];assert(recv(fd,h,2,MSG_WAITALL)==2&&h[0]==0x81&&(h[1]&128));size_t n=h[1]&127;if(n==126){uint8_t b[2];assert(recv(fd,b,2,MSG_WAITALL)==2);n=(size_t)b[0]*256+b[1];}assert(n<sizeof(raw));assert(recv(fd,mask,4,MSG_WAITALL)==4&&recv(fd,raw,n,MSG_WAITALL)==(ssize_t)n);for(size_t i=0;i<n;++i)raw[i]^=mask[i%4];return parse_json(raw,n);}
int main(int argc,char **argv) {
    assert(argc==2);char dir[]="/tmp/s12-mcp-unit-XXXXXX";assert(mkdtemp(dir));assert(!chmod(dir,0700));setenv("S12_TEST_PANEL_DIR",dir,1);panel_enabled=1;
    file("panel.control","v1 10 50 0 7 11\n");int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));struct wire w={0};w.fd=fds[0];w.deadline=now_ms()+5000;w.mcp_epoch=1;w.mcp_initialized=1;
    if(!strcmp(argv[1],"late-epoch-and-frame-guard")) {
        struct json_object *j=request(7,50);assert(!device_mcp_receive(&w,j));json_object_put(j);assert(volume_rpc.active);char old[33];strcpy(old,volume_rpc.nonce);
        session_wire_close(&w,0);close(fds[1]);assert(!volume_rpc.active);
        assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));w.fd=fds[0];w.deadline=now_ms()+5000;w.mcp_epoch=2;w.mcp_initialized=1;
        j=request(7,0);assert(!device_mcp_receive(&w,j));json_object_put(j);assert(volume_rpc.active&&strcmp(old,volume_rpc.nonce));
        char ack[160];snprintf(ack,sizeof(ack),"v1 %s ok 50 100 11\n",old);file("panel-volume.ack",ack);device_mcp_service();assert(volume_rpc.active);
        snprintf(ack,sizeof(ack),"v1 %s ok 0 0 12\n",volume_rpc.nonce);file("panel.control","v1 12 0 0 7 11\n");file("panel-volume.ack",ack);
        w.writing=1;device_mcp_service();assert(volume_rpc.active);struct pollfd p={fds[1],POLLIN,0};assert(!poll(&p,1,0));
        w.writing=0;device_mcp_service();assert(!volume_rpc.active&&panel.gain==0&&panel.cancel==7&&panel.manual==11&&!panel.muted);
        j=receive_reply(fds[1]);assert(j);struct json_object *rpc=field(j,"payload",json_type_object);assert(json_object_get_int(field(rpc,"id",json_type_int))==7);json_object_put(j);
    } else if(!strcmp(argv[1],"pending-cache-protected")) {
        struct json_object *j=request(7,50);assert(!device_mcp_receive(&w,j));json_object_put(j);unsigned slot=volume_rpc.slot;
        for(int i=8;i<28;++i){j=request(i,0);assert(!device_mcp_receive(&w,j));json_object_put(j);j=receive_reply(fds[1]);assert(j);json_object_put(j);}
        assert(!strcmp(w.mcp_cache[slot].id,"7")&&volume_rpc.active);
        char ack[160];snprintf(ack,sizeof(ack),"v1 %s ok 50 100 11\n",volume_rpc.nonce);file("panel.control","v1 11 100 0 7 11\n");file("panel-volume.ack",ack);device_mcp_service();j=receive_reply(fds[1]);assert(j);json_object_put(j);assert(w.mcp_cache[slot].result==1);
        j=request(7,50);assert(!device_mcp_receive(&w,j));json_object_put(j);assert(!volume_rpc.active);j=receive_reply(fds[1]);assert(j);json_object_put(j);
    } else if(!strcmp(argv[1],"idle-input-guards-and-preservation")) {
        assert(!session_keepalive_bind(&w,"mock-session"));
        const char *init="{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}}";
        size_t n=strlen(init),h=2;uint8_t head[4]={0x81,(uint8_t)n,0,0};if(n>=126){head[1]=126;head[2]=(uint8_t)(n>>8);head[3]=(uint8_t)n;h=4;}assert(send(fds[1],head,h,0)==(ssize_t)h&&send(fds[1],init,n,0)==(ssize_t)n);
        w.receiving=1;device_mcp_poll_input(&w);assert(!w.used);w.receiving=0;w.writing=1;device_mcp_poll_input(&w);assert(!w.used);
        w.writing=0;device_mcp_poll_input(&w);struct json_object *j=receive_reply(fds[1]);assert(j&&w.mcp_initialized==1);json_object_put(j);
        uint8_t audio[]={0x82,1,0x55};memcpy(w.pending,audio,sizeof(audio));w.pos=0;w.used=sizeof(audio);assert(!shutdown(fds[1],SHUT_WR));
        device_mcp_poll_input(&w);assert(w.mcp_deferred&&!session_keepalive.failed);
        struct message *m=malloc(sizeof(*m));assert(m);assert(!receive_message(&w,m)&&m->opcode==2&&m->len==1&&m->data[0]==0x55&&!w.receiving);free(m);
    } else if(!strcmp(argv[1],"safe-panel-parser")) {
        struct panel_control c;assert(!panel_parse("v1 5 0 1 7 11\n",&c)&&c.gain==0&&c.muted==1);assert(panel_parse("v1 5 201 1 7 11\n",&c));
    } else assert(!"unknown fixture");
    session_wire_close(&w,0);close(fds[1]);char path[4096];const char *names[]={"panel.control","panel-volume.request","panel-volume.ack"};for(unsigned i=0;i<3;++i){assert(!panel_path(path,sizeof(path),names[i]));unlink(path);}rmdir(dir);return 0;
}
'''

class MCPTest(unittest.TestCase):
    tearDownClass=classmethod(ClientTest.tearDownClass.__func__);invoke=ClientTest.invoke;wake_upload=ClientTest.wake_upload
    @classmethod
    def setUpClass(cls):
        ClientTest.setUpClass.__func__(cls);san=os.getenv('S12_TEST_SANITIZER','address,undefined')
        common=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fno-omit-frame-pointer','-fsanitize='+san]
        libs=[*native_lib_flags()]
        if os.getenv('S12_TEST_SANITIZER'):subprocess.run(common+['-DXIAOZHI_TEST_NO_ALSA','-DXIAOZHI_TEST_FAKE_CAPTURE',str(HERE/'xiaozhi_client.c'),str(HERE/'vendor_wake_test.c')]+libs+['-o',str(cls.binary)],check=True)
        source=cls.root/'mcp-harness.c';source.write_text(HARNESS);cls.harness=cls.root/'mcp-harness';subprocess.run(common+['-I',str(HERE),str(source)]+libs+['-o',str(cls.harness)],check=True)
        cls.panel_binary=cls.root/'panel-native';subprocess.run(common+['-DS12_PANEL_TEST',str(HERE/'panel_io.c'),'-o',str(cls.panel_binary)],check=True)
    def voice_case(self,handler,panel):
        def server(p):
            self.wake_upload(p,source='manual')
            self.assertIs(p.features.get('mcp'),True)
            handler(p)
            p.sock.sendall(frame(1,dict(type='tts',state='stop',session_id='mock-session')))
            self.assertEqual(p.receive()[0],8)
        env=dict(S12_TEST_CAPTURE_PROFILE='wake-array',S12_TEST_CAPTURE_FAST='1',S12_TEST_LOCAL_WAKE_FRAME='10',S12_TEST_PANEL_DIR=str(panel.root))
        r=self.invoke(server,['--local-wake','--panel','--wake-rounds','1'],env_change=env)
        self.assertEqual(r.returncode,0,r.stderr)
        return r

    def test_voice_discovery_volume_zero_and_deduplicated_requests(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            panel=FakePanel(td,muted=0)
            def server(p):
                initialize(p);call(p,3,'self.get_device_status',{})
                self.assertEqual(text_result(receive_rpc(p,3)),{'audio_speaker':{'volume':25}})
                for ident,volume in [(4,50),(5,0),(6,100)]:
                    call(p,ident,'self.audio_speaker.set_volume',{'volume':volume})
                    rpc=receive_rpc(p,ident);self.assertIs(text_result(rpc),True)
                    self.assertIs(rpc['result']['isError'],False)
                    call(p,ident,'self.audio_speaker.set_volume',{'volume':volume});self.assertEqual(receive_rpc(p,ident),rpc)
                    call(p,ident,'self.audio_speaker.set_volume',{'volume':49});self.assertEqual(receive_rpc(p,ident)['error']['code'],-32600)
            try:self.voice_case(server,panel)
            finally:panel.close()
            self.assertEqual([x[1] for x in panel.requests],[50,0,100])
            self.assertEqual((Path(td)/'panel.control').read_text().split()[2:],['200','0','7','11'])

    def test_voice_invalid_params_and_ids_never_submit_panel_write(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            panel=FakePanel(td,muted=0)
            def server(p):
                initialize(p)
                bad=[{},dict(volume=True),dict(volume=50.0),dict(volume='50'),dict(volume=-1),dict(volume=101),dict(volume=None),dict(volume=2**63),dict(volume=50,extra=1)]
                for ident,args in enumerate(bad,10):
                    call(p,ident,'self.audio_speaker.set_volume',args);self.assertEqual(receive_rpc(p,ident)['error']['code'],-32602)
                for ident in [None,True,{},2**53,'x'*129]:
                    call(p,ident,'self.audio_speaker.set_volume',{'volume':50});self.assertEqual(receive_rpc(p,None)['error']['code'],-32600)
                send_rpc(p,40,'unknown/method');self.assertEqual(receive_rpc(p,40)['error']['code'],-32601)
            try:self.voice_case(server,panel)
            finally:panel.close()
            self.assertEqual(panel.requests,[])

    def test_voice_failed_or_missing_ack_never_reports_success(self):
        for mode in ('failed','lost','expired'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(dir=self.root) as td:
                panel=FakePanel(td,muted=0,fail=mode=='failed',lost_ack=mode=='lost',delay=1.7 if mode=='expired' else 0)
                def server(p):
                    initialize(p);call(p,3,'self.audio_speaker.set_volume',{'volume':50})
                    rpc=receive_rpc(p,3);self.assertIs(rpc['result']['isError'],True)
                    self.assertNotEqual(rpc['result']['content'][0]['text'],'true')
                try:self.voice_case(server,panel)
                finally:panel.close()
                if mode=='expired':self.assertEqual((Path(td)/'panel.control').read_text().split()[2],'50')

    def test_voice_async_ack_preserves_mcp_ids_and_audio_flow(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            panel=FakePanel(td,muted=0,delay=.3)
            def server(p):
                self.wake_upload(p,source='manual');initialize(p)
                call(p,'volume-call','self.audio_speaker.set_volume',{'volume':0});call(p,'status-call','self.get_device_status',{})
                first=receive_rpc(p,'status-call');self.assertEqual(text_result(first),{'audio_speaker':{'volume':25}})
                second=receive_rpc(p,'volume-call');self.assertIs(text_result(second),True)
                p.sock.sendall(frame(1,dict(type='tts',state='start',session_id='mock-session'))+frame(2,self.packet)+frame(1,dict(type='tts',state='stop',session_id='mock-session')))
                self.assertEqual(p.receive()[0],8)
            env=dict(S12_TEST_CAPTURE_PROFILE='wake-array',S12_TEST_CAPTURE_FAST='1',S12_TEST_LOCAL_WAKE_FRAME='10',S12_TEST_PANEL_DIR=td)
            try:r=self.invoke(server,['--local-wake','--panel','--wake-rounds','1'],env_change=env)
            finally:panel.close()
            self.assertEqual(r.returncode,0,r.stderr);self.assertEqual((Path(td)/'panel.control').read_text().split()[2:],['0','0','7','11']);self.assertIn(b'turn_complete',r.stdout)
    def test_native_late_epoch_frame_barrier_and_pending_cache_bounds(self):
        for case in ('late-epoch-and-frame-guard','pending-cache-protected','idle-input-guards-and-preservation','safe-panel-parser'):
            with self.subTest(case=case):
                r=subprocess.run([str(self.harness),case],capture_output=True,timeout=4);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
if __name__=='__main__':unittest.main()
