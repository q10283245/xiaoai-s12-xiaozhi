#!/usr/bin/env python3
"""Native startup lifecycle/claim tests; no device, real audio or cloud access."""
import concurrent.futures
import json
import os
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import signal
import subprocess
import tempfile
import threading
import time
import unittest

import test_xiaozhi_client as protocol

HERE = Path(__file__).resolve().parent

HELPER_PREFIX = r'''
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define XIAOZHI_TEST_FAKE_CAPTURE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int local_capture_recovering;
static int stopping,panel_interrupted,panel_enabled=1;
static struct { int muted; } panel;
struct audio { int fixture; };
static int cue_calls,cue_result,ota_request,capture_closed=1;
static int64_t clock_ms;
static char ui[32];
static int64_t now_ms(void){return clock_ms;}
static void panel_state(const char *state){assert(strlen(state)<sizeof(ui));strcpy(ui,state);}
static void local_wake_state(const char *state){(void)state;}
static void panel_poll(void){}
static int ota_idle_requested(void){return ota_request;}
static int local_prompt(const char *name,const struct audio *options){assert(capture_closed);assert(!strcmp(name,"boot"));(void)options;++cue_calls;return cue_result;}
'''
HELPER_MAIN = r'''
int main(int argc,char **argv){
 assert(argc==2);
 if(!strcmp(argv[1],"setup")){
  startup_setup();printf("{\"enabled\":%d,\"pending\":%d,\"recovering\":%d}\n",startup_feedback.enabled,startup_feedback.pending,local_capture_recovering);
 }else if(!strcmp(argv[1],"claim"))printf("{\"claim\":%d}\n",startup_claim());
 else if(!strncmp(argv[1],"probe-",6)){
  startup_setup();assert(startup_feedback.enabled);struct audio a={0};
  cue_result=!strcmp(argv[1],"probe-fail")?-1:0;
  panel_interrupted=!strcmp(argv[1],"probe-cancel");stopping=!strcmp(argv[1],"probe-stop");
  ota_request=!strcmp(argv[1],"probe-ota");panel.muted=!strcmp(argv[1],"probe-mute");
  int skipped=panel_interrupted||stopping||ota_request||panel.muted;
  startup_after_probe(&a);assert(!startup_feedback.pending);assert(cue_calls==(skipped?0:1));
  assert(startup_feedback.ready_after_capture==(!skipped&&!cue_result));
  panel_interrupted=stopping=ota_request=panel.muted=0;startup_feedback.pending=1;
  startup_after_probe(&a);assert(cue_calls==(skipped?0:1));
  printf("{\"claimed_attempts\":1,\"cue_calls\":%d,\"repeat_suppressed\":true}\n",cue_calls);
 }else {
  startup_feedback.enabled=startup_feedback.ready_after_capture=1;clock_ms=1000;startup_idle_ui();assert(!strcmp(ui,"ready"));
  clock_ms=2999;startup_idle_ui();assert(!strcmp(ui,"ready"));clock_ms=3000;startup_idle_ui();assert(!strcmp(ui,"idle"));assert(!startup_feedback.ready_visible);
  startup_ready_clear();puts("{\"monotonic_ready_window\":true}");
 }
 return 0;
}
'''


class StartupFeedbackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s12-startup-offline-")
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / "client-native-no-alsa"
        subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-DXIAOZHI_TEST_NO_ALSA", "-DXIAOZHI_TEST_FAKE_CAPTURE",
                        str(HERE / "xiaozhi_client.c"), str(HERE / "vendor_wake_test.c"),
                        *native_lib_flags(), "-o", str(cls.binary)], check=True)
        source = (HERE / "xiaozhi_client.c").read_text()
        at = source.index("static struct {\n    int enabled,pending,ready_after_capture,ready_visible;")
        state = source[at:source.index("} startup_feedback;", at) + len("} startup_feedback;")]
        at = source.index("static int metadata_stat(")
        metadata = source[at:source.index("static int file_abi_check(", at)]
        at = source.index("static int startup_directory(")
        helpers = source[at:source.index("/* PID 1 pins", at)]
        at = source.index("static void startup_after_probe(")
        after = source[at:source.index("static void session_wire_close(", at)]
        c = cls.root / "startup-helpers.c"
        c.write_text(HELPER_PREFIX + state + metadata + helpers + after + HELPER_MAIN)
        cls.helpers = cls.root / "startup-helpers"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I",str(HERE), str(c), "-o", str(cls.helpers)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    invoke = protocol.ClientTest.invoke

    def helper(self, root, mode="setup"):
        env=dict(os.environ, S12_TEST_STARTUP_CONFIG_DIR=str(root), S12_TEST_PANEL_DIR=str(root),
                 ASAN_OPTIONS="detect_leaks=0")
        r=subprocess.run([str(self.helpers),mode],capture_output=True,text=True,env=env,timeout=3)
        self.assertEqual(r.returncode,0,r.stderr)
        return json.loads(r.stdout.splitlines()[-1])

    def opt_in(self, root):
        root.chmod(0o700)
        marker=root/"startup-feedback-enabled";marker.write_text("yes\n");marker.chmod(0o600)

    def env(self, root, **changes):
        return dict(S12_TEST_PANEL_DIR=str(root),S12_TEST_STARTUP_CONFIG_DIR=str(root),
                    S12_TEST_CAPTURE_PROFILE="wake-silence",S12_TEST_CAPTURE_FAST="0",
                    S12_TEST_AFE="healthy",**changes)

    @staticmethod
    def events(result):
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_absent_flag_and_malformed_or_nonprivate_flag_do_not_enable(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td)
            self.assertFalse(self.helper(root)["enabled"])
            for value,mode in [("yes",0o600),("yes\nextra",0o600),("no\n",0o600),("yes\n",0o644)]:
                marker=root/"startup-feedback-enabled";marker.write_text(value);marker.chmod(mode)
                self.assertFalse(self.helper(root)["enabled"])
            self.opt_in(root)
            self.assertEqual(self.helper(root),{"enabled":1,"pending":1,"recovering":1})

    def test_flag_and_runtime_directory_symlinks_hardlinks_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root);marker=root/"startup-feedback-enabled"
            target=root/"actual";marker.replace(target);marker.symlink_to(target)
            self.assertFalse(self.helper(root)["enabled"])
            marker.unlink();os.link(target,marker)
            self.assertFalse(self.helper(root)["enabled"])
            link=self.root/(root.name+"-link");link.symlink_to(root,target_is_directory=True)
            try:self.assertEqual(self.helper(link,"claim")["claim"],-1)
            finally:link.unlink()

    def test_claim_once_across_concurrent_processes_and_restarts(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td)
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results=list(pool.map(lambda _:self.helper(root,"claim")["claim"],range(16)))
            self.assertEqual(results.count(1),1)
            self.assertTrue(all(x in [-1,0,1] for x in results))
            self.assertEqual(self.helper(root,"claim")["claim"],0)
            claim=root/"startup-feedback.claimed"
            self.assertEqual(claim.read_bytes(),b"claimed\n")
            self.assertEqual(claim.stat().st_mode&0o777,0o600)

    def test_bad_claim_never_follows_or_overwrites_or_retries_audio(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);claim=root/"startup-feedback.claimed";target=root/"target"
            target.write_bytes(b"unchanged");target.chmod(0o600);claim.symlink_to(target)
            self.assertEqual(self.helper(root,"claim")["claim"],-1)
            self.assertEqual(target.read_bytes(),b"unchanged")
            claim.unlink();claim.write_bytes(b"");claim.chmod(0o600)
            self.assertEqual(self.helper(root,"claim")["claim"],-1)

    def test_ready_window_is_nonblocking_monotonic(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            self.assertTrue(self.helper(Path(td),"ui")["monotonic_ready_window"])

    def test_post_claim_play_failure_cancel_mute_ota_and_stop_never_repeat(self):
        for mode in ["probe-success","probe-fail","probe-cancel","probe-mute","probe-ota","probe-stop"]:
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(dir=self.root) as td:
                root=Path(td);self.opt_in(root)
                result=self.helper(root,mode)
                self.assertTrue(result["repeat_suppressed"])
                self.assertEqual((root/"startup-feedback.claimed").read_bytes(),b"claimed\n")

    def test_actual_first_chunk_join_and_close_precede_boot_then_capture_reopens(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root)
            r=self.invoke(args=["--local-wake","--panel"],env_change=self.env(root),interrupt_after=.55)
            self.assertEqual(r.returncode,0,r.stderr)
            events=self.events(r);names=[e["event"] for e in events]
            cue=next(i for i,e in enumerate(events) if e["event"]=="local_prompt" and e["prompt"]=="boot")
            first_close=next(e for e in events if e["event"]=="capture_reader_closed")
            self.assertTrue(first_close["reader_joined"])
            self.assertEqual(first_close["consumed_frames"],2880)
            self.assertLess(names.index("capture_reader_closed"),names.index("local_capture_closed"))
            self.assertLess(names.index("local_capture_closed"),cue)
            self.assertNotIn("local_wake_listening",names[:cue])
            self.assertIn("capture_reader_started",names[cue+1:])
            self.assertIn("local_wake_listening",names[cue+1:])
            self.assertEqual(sum(e["event"]=="local_prompt" and e["prompt"]=="boot" for e in events),1)
            r=self.invoke(args=["--local-wake","--panel"],env_change=self.env(root),interrupt_after=.35)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'"prompt":"boot"',r.stdout)

    def test_close_time_xrun_prevents_cue_and_ready_until_recovery(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root)
            env=self.env(root,S12_TEST_CAPTURE_FAULT="startup-close-xrun-once")
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=1)
            self.assertEqual(r.returncode,0,r.stderr)
            events=self.events(r);names=[e["event"] for e in events]
            recovery=names.index("local_capture_recovering")
            self.assertNotIn("local_prompt",names[:recovery])
            self.assertNotIn("local_wake_listening",names[:recovery])
            self.assertGreater(names.index("startup_feedback_complete"),recovery)
            self.assertEqual(sum(e["event"]=="local_prompt" and e["prompt"]=="boot" for e in events),1)

    def test_first_read_error_never_claims_or_announces_ready(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root)
            r=self.invoke(args=["--local-wake","--panel"],env_change=self.env(root,S12_TEST_CAPTURE_FAULT="io"))
            self.assertNotEqual(r.returncode,0)
            self.assertNotIn(b'local_prompt',r.stdout)
            self.assertNotIn(b'local_wake_listening',r.stdout)
            self.assertFalse((root/"startup-feedback.claimed").exists())

    def test_post_cue_capture_recovery_does_not_repeat_cue(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root)
            r=self.invoke(args=["--local-wake","--panel"],env_change=self.env(root,S12_TEST_CAPTURE_FAULT="startup-resume-xrun-once"),interrupt_after=.9)
            self.assertEqual(r.returncode,0,r.stderr)
            events=self.events(r);names=[e["event"] for e in events]
            self.assertEqual(sum(e["event"]=="local_prompt" and e["prompt"]=="boot" for e in events),1)
            recovery=names.index("local_capture_recovering")
            self.assertNotIn("local_wake_listening",names[:recovery])
            self.assertGreater(names.index("local_wake_listening"),recovery)

    def test_missing_afe_preserves_voice_fallback_but_has_no_startup_cue(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root);env=self.env(root);env.pop("S12_TEST_AFE")
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.35)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn(b'frontend_unavailable',r.stdout)
            self.assertIn(b'local_wake_listening',r.stdout)
            self.assertNotIn(b'"prompt":"boot"',r.stdout)

    def test_ready_window_does_not_block_keyword_or_override_listening(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root)
            env=self.env(root,S12_TEST_LOCAL_WAKE_FRAME="10")
            started=time.monotonic()
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=1.2)
            self.assertLess(time.monotonic()-started,1.8)
            self.assertEqual(r.returncode,0,r.stderr)
            names=[e["event"] for e in self.events(r)]
            self.assertLess(names.index("startup_feedback_complete"),names.index("local_wake_detected"))
            self.assertIn("wake_listening",names)

    def test_ready_visual_expires_while_capture_keeps_running(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);self.opt_in(root);seen=[];stop=threading.Event()
            def observer():
                last=None
                while not stop.is_set():
                    path=root/"ui.state"
                    if path.exists():
                        value=path.read_text()
                        if value!=last:seen.append((value,time.monotonic()));last=value
                    time.sleep(.005)
            worker=threading.Thread(target=observer);worker.start()
            r=self.invoke(args=["--local-wake","--panel"],env_change=self.env(root),interrupt_after=2.5)
            stop.set();worker.join()
            self.assertEqual(r.returncode,0,r.stderr)
            ready=next(t for s,t in seen if s=="ready\n")
            idle=next(t for s,t in seen if s=="idle\n" and t>ready)
            self.assertGreater(idle-ready,1.8);self.assertLess(idle-ready,2.25)


if __name__=="__main__":unittest.main()
