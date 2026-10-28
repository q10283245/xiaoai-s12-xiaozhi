#!/usr/bin/env python3
"""Offline protocol tests. Only ephemeral 127.0.0.1 mock servers are used.

The native test binary links Homebrew json-c/Opus but has ALSA compiled out.
No speaker, production server, account, token store or serial port is touched.
"""
import base64
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
from test_xiaozhi_dependencies import native_lib_flags, opus_library
import socket
import signal
import struct
import subprocess
import tempfile
import threading
import time
import unittest

HERE = Path(__file__).resolve().parent
FAKE_TOKEN = "offline-test-token-do-not-use"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def frame(op, payload=b"", final=True):
    if isinstance(payload, dict):
        payload = json.dumps(payload).encode()
    head = bytes([(0x80 if final else 0) | op])
    if len(payload) < 126:
        head += bytes([len(payload)])
    elif len(payload) <= 65535:
        head += b"\x7e" + struct.pack("!H", len(payload))
    else:
        head += b"\x7f" + struct.pack("!Q", len(payload))
    return head + payload


def hello(**overrides):
    result = {"type": "hello", "transport": "websocket", "session_id": "mock-session",
              "audio_params": {"format": "opus", "sample_rate": 24000,
                               "channels": 1, "frame_duration": 60}}
    result.update(overrides)
    return result


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()

    def exact(self, n):
        while len(self.buf) < n:
            data = self.sock.recv(4096)
            if not data:
                raise EOFError("client closed")
            self.buf.extend(data)
        data = bytes(self.buf[:n])
        del self.buf[:n]
        return data

    def http(self):
        while b"\r\n\r\n" not in self.buf:
            self.buf.extend(self.sock.recv(4096))
            if len(self.buf) > 16384:
                raise AssertionError("request oversized")
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        self.buf = bytearray(rest)
        rows = head.decode().split("\r\n")
        assert rows[0] == "GET /xiaozhi/v1/ HTTP/1.1"
        headers = dict(x.split(": ", 1) for x in rows[1:])
        assert headers["Authorization"] == "Bearer " + FAKE_TOKEN
        assert headers["Protocol-Version"] == "1"
        assert headers["Device-Id"] == "02:00:00:00:00:01"
        assert headers["Client-Id"] == "offline-client"
        return headers

    def upgrade(self, extra=b"", bad_accept=False):
        headers = self.http()
        accept = base64.b64encode(hashlib.sha1((headers["Sec-WebSocket-Key"] + GUID).encode()).digest())
        if bad_accept:
            accept = b"invalid-accept"
        self.sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          b"Connection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: " + accept +
                          b"\r\n\r\n" + extra)

    def receive(self, include_ping=False):
        a, b = self.exact(2)
        assert a & 0x80, "client messages must be final"
        assert b & 0x80, "client must mask every frame"
        n = b & 127
        if n == 126:
            n = struct.unpack("!H", self.exact(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self.exact(8))[0]
        assert n <= 65536
        mask = self.exact(4)
        payload = self.exact(n)
        data=bytes(x ^ mask[i % 4] for i, x in enumerate(payload))
        if a & 15 == 9 and not include_ping:
            self.sock.sendall(frame(10,data))
            return self.receive(include_ping=include_ping)
        return a & 15, data

    def client_hello(self):
        op, data = self.receive()
        assert op == 1
        msg = json.loads(data)
        features=msg.pop("features",None)
        self.features=features
        if features is not None:
            assert all(value is True for value in features.values())
            assert set(features)<={"mcp","dialogue_state","local_command_feedback","quiet_followup","capture_diagnostics"}
        assert msg == {"type": "hello", "version": 1, "transport": "websocket",
                       "audio_params": {"format": "opus", "sample_rate": 16000,
                                        "channels": 1, "frame_duration": 60}}

    def normal_hello(self):
        self.upgrade()
        self.client_hello()
        self.sock.sendall(frame(1, hello()))


class Mock:
    def __init__(self, handler, connections=1,accept_timeout=4):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(1)
        self.listener.settimeout(accept_timeout)
        self.error = None

        def run():
            try:
                for _ in range(connections):
                    sock, _ = self.listener.accept()
                    with sock:
                        sock.settimeout(4)
                        handler(Peer(sock))
            except (BrokenPipeError, ConnectionResetError):
                pass  # Expected when the client rejects an invalid response.
            except Exception as ex:
                self.error = ex
            finally:
                self.listener.close()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def finish(self):
        self.thread.join(6)
        if self.thread.is_alive():
            raise AssertionError("mock server did not stop")
        if self.error:
            raise self.error


def silence_packet():
    """Encode synthetic silence in RAM; never reads a microphone."""
    lib = ctypes.CDLL(opus_library())
    lib.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_int)]
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                               ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int]
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    err = ctypes.c_int()
    encoder = lib.opus_encoder_create(24000, 1, 2048, ctypes.byref(err))
    if not encoder or err.value:
        raise RuntimeError("synthetic Opus encoder failed")
    pcm = (ctypes.c_int16 * 1440)()
    encoded = (ctypes.c_ubyte * 1275)()
    try:
        n = lib.opus_encode(encoder, pcm, 1440, encoded, len(encoded))
        if n <= 0:
            raise RuntimeError("synthetic Opus encode failed")
        return bytes(encoded[:n])
    finally:
        lib.opus_encoder_destroy(encoder)


def decode_uplink(packets):
    lib = ctypes.CDLL(opus_library())
    lib.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                               ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
    error = ctypes.c_int()
    decoder = lib.opus_decoder_create(16000, 1, ctypes.byref(error))
    if not decoder or error.value:
        raise RuntimeError("offline decoder creation failed")
    samples = []
    try:
        for packet in packets:
            out = (ctypes.c_int16 * 960)()
            n = lib.opus_decode(decoder, packet, len(packet), out, 960, 0)
            if n != 960:
                raise AssertionError("uplink packet is not exactly 60 ms mono")
            samples.extend(out[:n])
        return samples
    finally:
        lib.opus_decoder_destroy(decoder)


class ClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="s12-client-offline-")
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / "client-native-no-alsa"
        subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-DXIAOZHI_TEST_NO_ALSA", "-DXIAOZHI_TEST_FAKE_CAPTURE", str(HERE / "xiaozhi_client.c"),
                        str(HERE / "vendor_wake_test.c"),
                        *native_lib_flags(), "-o", str(cls.binary)], check=True)
        cls.packet = silence_packet()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, handler=None, args=None, config_change=None, token_bytes=None, mode=0o600, env_change=None,
               mock_connections=1, interrupt_after=None,mock_accept_timeout=4,stop_event=None,stop_on_event=None):
        mock = Mock(handler, connections=mock_connections,accept_timeout=mock_accept_timeout) if handler else None
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            token = Path(td) / "fake-token"
            token.write_bytes(token_bytes if token_bytes is not None else FAKE_TOKEN.encode() + b"\n")
            token.chmod(mode)
            cfg = {"url": f"ws://127.0.0.1:{mock.port if mock else 9}/xiaozhi/v1/",
                   "deviceid": "02:00:00:00:00:01", "clientid": "offline-client",
                   "token-file": str(token)}
            if config_change:
                cfg.update(config_change)
            path = Path(td) / "fake-config.json"
            path.write_text(json.dumps(cfg))
            command = [str(self.binary), "--config", str(path), "--timeout", "2"]
            command += args if args is not None else ["--check-ws"]
            env = os.environ.copy()
            if env_change:
                env.update(env_change)
            if stop_on_event is not None:
                # Negative acoustic fixtures finish on consumed-input events,
                # never on an assumed host CPU speed. Drain both pipes while
                # waiting so verbose diagnostics cannot stall the fake reader.
                process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
                streams=[[],[]];matched=threading.Event()
                name,required=stop_on_event if isinstance(stop_on_event,tuple) else (stop_on_event,1)
                def collect(pipe,index):
                    count=0
                    for line in iter(pipe.readline,b''):
                        streams[index].append(line)
                        if index==0:
                            try:event=json.loads(line)
                            except (ValueError,UnicodeError):continue
                            if event.get('event')==name:
                                count+=1
                                if count>=required:matched.set()
                readers=[threading.Thread(target=collect,args=(process.stdout,0)),threading.Thread(target=collect,args=(process.stderr,1))]
                for reader in readers:reader.start()
                until=time.monotonic()+(interrupt_after if interrupt_after is not None else 12)
                while not matched.wait(.025) and process.poll() is None and time.monotonic()<until:pass
                if process.poll() is None:process.send_signal(signal.SIGTERM)
                process.wait(timeout=3)
                for reader in readers:reader.join(3)
                process.stdout.close();process.stderr.close()
                self.assertTrue(all(not reader.is_alive() for reader in readers),'fixture pipe reader did not close')
                result=subprocess.CompletedProcess(command,process.returncode,b''.join(streams[0]),b''.join(streams[1]))
                self.assertTrue(matched.is_set(),f'bounded fixture event {name} was not observed: '+result.stdout[-3000:].decode(errors='replace'))
            elif interrupt_after is None:
                result = subprocess.run(command, capture_output=True, timeout=18, env=env)
            else:
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                if stop_event is None:time.sleep(interrupt_after)
                else:stop_event.wait(interrupt_after)
                if process.poll() is None:process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=3)
                result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
        if mock:
            mock.finish()
        self.assertNotIn(FAKE_TOKEN.encode(), result.stdout + result.stderr)
        return result

    def test_selftest(self):
        r = subprocess.run([str(self.binary), "--selftest"], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_file_abi_diagnostic_is_metadata_only_and_rejects_symlink(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            fixture = Path(td) / "fake-private-file"
            fixture.write_text(FAKE_TOKEN)
            fixture.chmod(0o600)
            r = subprocess.run([str(self.binary), "--file-abi-check", str(fixture)], capture_output=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn(FAKE_TOKEN.encode(), r.stdout + r.stderr)
            metadata = json.loads(r.stdout)
            self.assertEqual(metadata["size"], len(FAKE_TOKEN))
            self.assertEqual(metadata["uid"], os.geteuid())
            self.assertTrue(metadata["regular"])
            link = Path(td) / "symlink"
            link.symlink_to(fixture)
            r = subprocess.run([str(self.binary), "--file-abi-check", str(link)], capture_output=True)
            self.assertNotEqual(r.returncode, 0)

    def test_check_and_masked_client_close(self):
        def server(p):
            p.normal_hello()
            op, data = p.receive()
            self.assertEqual((op, data), (8, b"\x03\xe8"))
        r = self.invoke(server)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b'"capture_implemented":true', r.stdout)

    def test_pipelined_ping_fragmented_hello_partial_tcp(self):
        def server(p):
            p.upgrade(frame(9, b"test-ping"))
            p.client_hello()
            self.assertEqual(p.receive(), (10, b"test-ping"))
            data = json.dumps(hello()).encode()
            content = frame(1, data[:22], False) + frame(9, b"between") + frame(0, data[22:])
            for byte in content:
                p.sock.sendall(bytes([byte]))
            self.assertEqual(p.receive(), (10, b"between"))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_text_json_escaping_opus_resample_and_tts(self):
        text = '你好，"引号" \\ slash\nnext'
        def server(p):
            p.normal_hello()
            op, data = p.receive()
            self.assertEqual(op, 1)
            self.assertEqual(json.loads(data), {"session_id": "mock-session", "type": "listen",
                                               "state": "detect", "text": text})
            p.sock.sendall(frame(1, {"session_id": "mock-session", "type": "tts", "state": "start"}) +
                           frame(1, {"type": "system", "command": "reboot"}) +
                           frame(2, self.packet) + frame(2, self.packet) +
                           frame(1, {"session_id": "mock-session", "type": "tts", "state": "stop"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--text", text])
        self.assertEqual(r.returncode, 0, r.stderr)
        done = json.loads(r.stdout.splitlines()[-1])
        self.assertEqual(done["opus_packets"], 2)
        self.assertEqual(done["decoded_frames_48000"], 5760)
        self.assertFalse(done["playback"])

    def test_reject_bad_upgrade_accept(self):
        r = self.invoke(lambda p: p.upgrade(bad_accept=True))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"invalid WebSocket accept", r.stderr)

    def test_reject_masked_or_oversized_or_wrong_continuation(self):
        cases = [b"\x81\x80", b"\x81\x7f" + struct.pack("!Q", 65537), frame(0, b"orphan")]
        for bad in cases:
            with self.subTest(frame=bad):
                def server(p):
                    p.upgrade(); p.client_hello(); p.sock.sendall(bad)
                r = self.invoke(server)
                self.assertNotEqual(r.returncode, 0)

    def test_reject_trailing_json_and_bad_utf8(self):
        for suffix in [b" {}", b" garbage", b"\xff"]:
            with self.subTest(suffix=suffix):
                def server(p):
                    p.upgrade(); p.client_hello()
                    p.sock.sendall(frame(1, json.dumps(hello()).encode() + suffix))
                self.assertNotEqual(self.invoke(server).returncode, 0)

    def test_reject_wrong_hello_audio_params(self):
        def server(p):
            p.upgrade(); p.client_hello()
            p.sock.sendall(frame(1, hello(audio_params={"format": "opus", "sample_rate": 44100, "channels": 1})))
        self.assertNotEqual(self.invoke(server).returncode, 0)

    def test_no_tts_start_or_session_mismatch(self):
        for bad in [frame(2, self.packet), frame(1, {"type": "tts", "state": "start", "session_id": "other"})]:
            def server(p):
                p.normal_hello(); p.receive(); p.sock.sendall(bad)
            self.assertNotEqual(self.invoke(server, ["--text", "mock question"]).returncode, 0)

    def test_uploaded_audio_matching_stop_only_is_neutral_terminal(self):
        def server(p):
            self.wake_upload(p,source="manual")
            p.sock.sendall(frame(1,{"type":"tts","state":"stop","session_id":"mock-session"}))
            self.assertEqual(p.receive()[0],8)
        env=self.wake_env("wake-array");env.update(S12_TEST_LOCAL_WAKE_FRAME="10")
        r=self.invoke(server,["--local-wake","--wake-rounds","1"],env_change=env)
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        terminal=next(e for e in events if e["event"]=="no_audio_terminal")
        self.assertEqual(terminal["reason"],"unknown")
        self.assertNotIn("turn_complete",[e["event"] for e in events])
        self.assertEqual([e["prompt"] for e in events if e["event"]=="local_prompt"],["wake"])

    def test_stop_only_requires_audio_authorization_and_exact_session(self):
        for session in [None,"","other",12,"mock-session\u0000other"]:
            with self.subTest(session=session):
                def server(p):
                    self.wake_upload(p,source="manual")
                    msg={"type":"tts","state":"stop"}
                    if session is not None:msg["session_id"]=session
                    p.sock.sendall(frame(1,msg))
                env=self.wake_env("wake-array");env.update(S12_TEST_LOCAL_WAKE_FRAME="10")
                r=self.invoke(server,["--local-wake","--wake-rounds","1"],env_change=env)
                self.assertNotEqual(r.returncode,0)
                self.assertNotIn(b'no_audio_terminal',r.stdout)
        def text_server(p):
            p.normal_hello();p.receive()
            p.sock.sendall(frame(1,{"type":"tts","state":"stop","session_id":"mock-session"}))
        self.assertNotEqual(self.invoke(text_server,["--text","mock question"]).returncode,0)

    def test_stop_only_does_not_hide_tts_activity_audio_or_explicit_error(self):
        for prior in [frame(1,{"type":"tts","state":"sentence_start","session_id":"mock-session"}),
                      frame(1,{"type":"tts","state":"sentence_end","session_id":"mock-session"}),
                      frame(1,{"type":"tts","state":"unknown","session_id":"mock-session"}),
                      frame(2,self.packet),frame(1,{"type":"error"}),frame(1,{"type":"goodbye"}),
                      frame(1,{"type":"wake","state":"error","session_id":"mock-session"})]:
            with self.subTest(prior=prior):
                def server(p):
                    self.wake_upload(p,source="manual")
                    p.sock.sendall(prior+frame(1,{"type":"tts","state":"stop","session_id":"mock-session"}))
                env=self.wake_env("wake-array");env.update(S12_TEST_LOCAL_WAKE_FRAME="10")
                r=self.invoke(server,["--local-wake","--wake-rounds","1"],env_change=env)
                self.assertNotEqual(r.returncode,0)
                self.assertNotIn(b'no_audio_terminal',r.stdout)

    def test_buffer_tts_delayed_audio_and_stop(self):
        def server(p):
            p.normal_hello(); p.receive()
            p.sock.sendall(frame(1, {"type": "tts", "state": "start"}))
            for _ in range(3):
                p.sock.sendall(frame(2, self.packet))
                time.sleep(0.04)
            time.sleep(0.35)  # Late stop must not create a playback gap.
            p.sock.sendall(frame(1, {"type": "tts", "state": "stop"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--text", "mock question", "--buffer-tts"])
        self.assertEqual(r.returncode, 0, r.stderr)
        events = [json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual([x["event"] for x in events], ["hello", "tts_start", "tts_buffered", "turn_complete"])
        self.assertEqual(events[2]["compressed_bytes"], 3 * len(self.packet))
        self.assertEqual(events[-1]["opus_packets"], 3)
        self.assertEqual(events[-1]["decoded_frames_48000"], 8640)
        self.assertTrue(events[-1]["buffered"])

    def test_buffer_tts_does_not_decode_before_complete_stop(self):
        def server(p):
            p.normal_hello(); p.receive()
            # This is an invalid Opus packet. Reaching the following ping/pong
            # proves it was buffered rather than decoded before TTS stop.
            p.sock.sendall(frame(1, {"type": "tts", "state": "start"}) +
                           frame(2, b"\x03") + frame(9, b"waiting-for-stop"))
            self.assertEqual(p.receive(), (10, b"waiting-for-stop"))
            time.sleep(0.1)
            p.sock.sendall(frame(1, {"type": "tts", "state": "stop"}))
        r = self.invoke(server, ["--text", "mock question", "--buffer-tts"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"tts_buffered", r.stdout)
        self.assertIn(b"Opus packet decode failed", r.stderr)
        self.assertNotIn(b"turn_complete", r.stdout)

    def test_buffer_tts_packet_and_total_byte_limits(self):
        for payload, count in [(self.packet, 2001), (bytes(4096), 129), (bytes(4097), 1)]:
            with self.subTest(packet_bytes=len(payload), count=count):
                def server(p):
                    p.normal_hello(); p.receive()
                    p.sock.sendall(frame(1, {"type": "tts", "state": "start"}))
                    for _ in range(count):
                        p.sock.sendall(frame(2, payload))
                r = self.invoke(server, ["--text", "mock question", "--buffer-tts"])
                self.assertNotEqual(r.returncode, 0)
                self.assertNotIn(b"turn_complete", r.stdout)
                self.assertIn(b"buffer", r.stderr)

    def test_buffer_tts_without_stop_times_out(self):
        def server(p):
            p.normal_hello(); p.receive()
            p.sock.sendall(frame(1, {"type": "tts", "state": "start"}) + frame(2, self.packet))
            time.sleep(2.2)
        r = self.invoke(server, ["--text", "mock question", "--buffer-tts"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"deadline expired", r.stderr)
        self.assertNotIn(b"tts_buffered", r.stdout)
        self.assertNotIn(b"turn_complete", r.stdout)

    def test_token_permissions_injection_and_url_schemes(self):
        for mode in [0o644, 0o666]:
            self.assertNotEqual(self.invoke(mode=mode).returncode, 0)
        for token in [b"x\r\nInjected: yes\n", b"\n", b"x\x00y"]:
            self.assertNotEqual(self.invoke(token_bytes=token).returncode, 0)
        for url in ["wss://127.0.0.1/", "http://127.0.0.1/", "ws://user@127.0.0.1/",
                    "ws://127.0.0.1/\r\nInjected:yes", "ws://127.0.0.1:0/", "ws://8.8.8.8/"]:
            self.assertNotEqual(self.invoke(config_change={"url": url}).returncode, 0)
        self.assertNotEqual(self.invoke(config_change={"clientid": "bad\r\nInjected"}).returncode, 0)

    def test_standard_hello_optional_audio_and_session(self):
        for response in [{"type":"hello","transport":"websocket"},
                         {"type":"hello","transport":"websocket","session_id":""},
                         {"type":"hello","transport":"websocket","audio_params":{"format":"opus"}}]:
            with self.subTest(response=response):
                def server(p):
                    p.upgrade();p.client_hello();p.sock.sendall(frame(1,response));self.assertEqual(p.receive()[0],8)
                r=self.invoke(server)
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertIn(b'"server_sample_rate":24000',r.stdout)

    def test_standard_hello_explicit_invalid_optional_fields_rejected(self):
        for fields in [{"session_id":42},{"audio_params":[]},
                       {"audio_params":{"sample_rate":"24000"}},
                       {"audio_params":{"format":"pcm"}},
                       {"audio_params":{"channels":2}}]:
            with self.subTest(fields=fields):
                def server(p):
                    p.upgrade();p.client_hello();p.sock.sendall(frame(1,dict(type="hello",transport="websocket",**fields)))
                self.assertNotEqual(self.invoke(server).returncode,0)

    def test_standard_audio_upload_paced_without_private_queue_barrier(self):
        observations=[]
        def server(p):
            p.normal_hello();op,data=p.receive();self.assertEqual(op,1)
            self.assertEqual(json.loads(data)['state'],'start');times=[]
            while True:
                op,data=p.receive()
                if op==1:
                    stop=json.loads(data);self.assertEqual(stop,{"type":"listen","state":"stop","session_id":"mock-session"})
                    self.assertGreaterEqual(time.monotonic()-times[0],(len(times)-1)*.06-.02)
                    gaps=sorted(b-a for a,b in zip(times,times[1:]))
                    self.assertGreaterEqual(gaps[len(gaps)//2],.035)
                    break
                self.assertEqual(op,2);times.append(time.monotonic())
            observations.append(len(times))
            p.sock.sendall(frame(1,{"type":"tts","state":"stop","session_id":"mock-session"}))
            self.assertEqual(p.receive()[0],8)
        env=dict(self.wake_env('wake-burst'),S12_TEST_REALTIME_UPLINK='1')
        r=self.invoke(server,["--wake-loop","--wake-rounds","1"],env_change=env)
        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(len(observations),1);self.assertGreater(observations[0],2)

    def test_deadline_without_server_hello(self):
        def server(p):
            p.upgrade(); p.client_hello(); time.sleep(2.2)
        r = self.invoke(server)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"deadline expired", r.stderr)

    def capture_case(self, channel, seconds):
        packets = []
        elapsed = []
        def server(p):
            p.normal_hello()
            op, data = p.receive()
            self.assertEqual(op, 1)
            self.assertEqual(json.loads(data), {"session_id": "mock-session", "type": "listen",
                                               "state": "start", "mode": "manual"})
            start = time.monotonic()
            while True:
                op, data = p.receive()
                if op == 1:
                    self.assertEqual(json.loads(data), {"session_id": "mock-session", "type": "listen", "state": "stop"})
                    break
                self.assertEqual(op, 2)
                packets.append(data)
            elapsed.append(time.monotonic() - start)
            p.sock.sendall(frame(1, {"type": "tts", "state": "start"}) + frame(2, self.packet) +
                           frame(1, {"type": "tts", "state": "stop"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--listen-once", "--mic-channel", str(channel), "--seconds", str(seconds)])
        self.assertEqual(r.returncode, 0, r.stderr)
        events = [json.loads(x) for x in r.stdout.splitlines()]
        capture = next(e for e in events if e["event"] == "recording_complete")
        expected_packets = math.ceil(seconds * 16000 / 960)
        self.assertEqual(len(packets), expected_packets)
        self.assertEqual(capture["captured_frames_48000"], seconds * 48000)
        self.assertEqual(capture["uplink_samples_16000"], seconds * 16000)
        self.assertEqual(capture["zero_padding_samples"], expected_packets * 960 - seconds * 16000)
        self.assertGreater(capture["short_reads_assembled"], 0)
        self.assertGreater(elapsed[0], seconds - 0.3)
        self.assertLess(elapsed[0], seconds + 1.5)
        self.assertEqual(events[-1]["event"], "turn_complete")
        return decode_uplink(packets)

    def test_listen_once_real_opus_paced_3s_tone(self):
        samples = self.capture_case(0, 3)[4000:]
        rms = math.sqrt(sum(x * x for x in samples) / len(samples))
        self.assertGreater(rms, 4000)
        self.assertLess(rms, 7000)
        crossings = sum(a <= 0 < b for a, b in zip(samples, samples[1:]))
        frequency = crossings * 16000 / len(samples)
        self.assertAlmostEqual(frequency, 1000, delta=30)

    def test_listen_once_4s_padding_and_antialias_channel_selection(self):
        # Fake channel 1 is 12 kHz. It must not alias into loud 4 kHz speech.
        # Fake channel 0 simultaneously carries loud 1 kHz, so this also proves
        # that the selected channel is used instead of accidentally mixing all.
        samples = self.capture_case(1, 4)[4000:-960]
        rms = math.sqrt(sum(x * x for x in samples) / len(samples))
        self.assertLess(rms, 100)

    def test_listen_once_xrun_fails_without_false_stop_or_completion(self):
        seen = []
        def server(p):
            p.normal_hello()
            seen.append(p.receive())  # manual start
            try:
                while True:
                    seen.append(p.receive())
            except EOFError:
                return
        r = self.invoke(server, ["--listen-once", "--seconds", "3"],
                        env_change={"S12_TEST_CAPTURE_FAULT": "xrun"})
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"XRUN", r.stderr)
        self.assertNotIn(b"recording_complete", r.stdout)
        self.assertNotIn(b"turn_complete", r.stdout)
        self.assertTrue(all(op == 2 for op, _ in seen[1:]))

    def wake_upload(self, p, source="s12_wake"):
        """The standard stop follows the complete ordered Opus frame sequence."""
        p.normal_hello()
        op, data = p.receive()
        self.assertEqual(op, 1)
        self.assertEqual(json.loads(data), {"session_id": "mock-session", "type": "listen",
                                           "state": "start", "mode": "manual", "source": source})
        start = time.monotonic()
        packets = []
        while True:
            op, data = p.receive()
            if op == 1:
                self.assertEqual(json.loads(data), {"session_id": "mock-session", "type": "listen",
                                                   "state": "stop"})
                self.assertGreater(len(packets), 0)
                self.assertLessEqual(len(packets), 200)
                return packets, time.monotonic() - start
            self.assertEqual(op, 2)
            packets.append(data)

    @staticmethod
    def wake_env(profile="wake-burst"):
        return {"S12_TEST_CAPTURE_PROFILE": profile, "S12_TEST_CAPTURE_FAST": "1"}

    def test_panel_manual_button_uses_manual_source_and_state_file(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            directory=Path(td)
            control=directory/'panel.control'
            control.write_text('v1 1 60 0 1 1\n');control.chmod(0o600)
            pressed=[]
            def press():
                until=time.monotonic()+2
                while time.monotonic()<until:
                    state=directory/'local-wake.state'
                    if state.exists() and state.read_text()=='ready\n':
                        new=directory/'new';new.write_text('v1 2 60 0 1 2\n');new.chmod(0o600);new.replace(control);pressed.append(True);return
                    time.sleep(.005)
            def server(p):
                self.wake_upload(p,source='manual')
                p.sock.sendall(frame(1,{'type':'tts','state':'start'})+frame(1,{'type':'tts','state':'stop'}))
                self.assertEqual(p.receive()[0],8)
            env=self.wake_env();env['S12_TEST_PANEL_DIR']=td
            worker=threading.Thread(target=press);worker.start()
            r=self.invoke(server,['--local-wake','--wake-rounds','1','--panel'],env_change=env)
            worker.join(1);self.assertEqual(pressed,[True])
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual((directory/'ui.state').read_text(),'idle\n')

    def test_panel_cancel_interrupts_waiting_server_without_retrying_audio(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            directory=Path(td)
            control=directory/'panel.control'
            control.write_text('v1 0 50 0 0 0\n');control.chmod(0o600)
            def server(p):
                self.wake_upload(p)
                new=directory/'new';new.write_text('v1 1 50 0 1 0\n');new.chmod(0o600);new.replace(control)
                op,body=p.receive()
                self.assertEqual(op,1)
                self.assertEqual(json.loads(body),{'type':'abort','session_id':'mock-session'})
            env=self.wake_env();env['S12_TEST_PANEL_DIR']=td
            r=self.invoke(server,['--wake-loop','--wake-rounds','1','--panel'],env_change=env)
            self.assertEqual(r.returncode,0,r.stderr)
            events=[json.loads(line)['event'] for line in r.stdout.splitlines()]
            self.assertIn('panel_turn_cancelled',events)
            self.assertNotIn('wake_retry',events)

    def test_panel_mute_keeps_microphone_closed_until_unmuted(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            directory=Path(td)
            control=directory/'panel.control'
            control.write_text('v1 0 50 1 0 0\n');control.chmod(0o600)
            env=self.wake_env();env['S12_TEST_PANEL_DIR']=td
            r=self.invoke(args=['--wake-loop','--panel'],env_change=env,interrupt_after=.25)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'wake_listening',r.stdout)
            self.assertNotIn(b'utterance_uploaded',r.stdout)
            self.assertEqual((directory/'ui.state').read_text(),'muted\n')

    def test_wake_ignored_immediately_relistens_then_tts_without_accepted_event(self):
        rounds = []
        arrivals = []
        def server(p):
            arrivals.append(time.monotonic())
            packets, _ = self.wake_upload(p)
            rounds.append(packets)
            if len(rounds) == 1:
                p.sock.sendall(frame(1, {"type": "wake", "state": "ignored"}))
            else:
                p.sock.sendall(frame(1, {"type": "tts", "state": "start"}) +
                               frame(2, self.packet) + frame(2, self.packet) +
                               frame(1, {"type": "tts", "state": "stop"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "2"],
                        env_change=self.wake_env(), mock_connections=2)
        self.assertEqual(r.returncode, 0, r.stderr)
        events = [json.loads(line) for line in r.stdout.splitlines()]
        names = [e["event"] for e in events]
        self.assertEqual(names.count("wake_listening"), 2)
        self.assertEqual(names.count("wake_ignored"), 1)
        self.assertEqual(names.count("turn_complete"), 1)
        self.assertNotIn("wake_retry", names)
        self.assertLess(arrivals[1] - arrivals[0], 1.0)
        self.assertEqual(events[-1]["submitted_rounds"], 2)
        complete = [e for e in events if e["event"] == "turn_complete"][0]
        self.assertTrue(complete["buffered"])
        self.assertEqual(complete["opus_packets"], 2)
        onsets = [e for e in events if e["event"] == "speech_detected"]
        self.assertEqual([e["preroll_ms"] for e in onsets], [240, 240])
        self.assertEqual(len(decode_uplink(rounds[0])), len(rounds[0]) * 960)

    def test_local_wake_non_keyword_speech_never_contacts_server(self):
        # The deterministic spotter does not fire even though capture supplies
        # speech. Token failure must not disable the local offline listener.
        r=self.invoke(args=["--local-wake"],env_change=self.wake_env(),token_bytes=b"",interrupt_after=.35)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn(b'"local_keyword_spotting":true',r.stdout)
        self.assertIn(b'"idle_audio_uploaded":false',r.stdout)
        self.assertNotIn(b'"hello"',r.stdout)
        self.assertNotIn(b'utterance_uploaded',r.stdout)
        self.assertNotIn(b'local_prompt',r.stdout)

    def test_local_wake_then_local_reply_uploads_manual_command(self):
        seen=[]
        def server(p):
            data,_=self.wake_upload(p,source="manual");seen.extend(data)
            p.sock.sendall(frame(1,{"type":"tts","state":"start"})+frame(2,self.packet)+
                           frame(1,{"type":"tts","state":"stop"}))
            self.assertEqual(p.receive()[0],8)
        env=self.wake_env();env["S12_TEST_LOCAL_WAKE_FRAME"]="10"
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            env["S12_TEST_PANEL_DIR"]=td
            r=self.invoke(server,["--local-wake","--panel","--wake-rounds","1"],env_change=env)
            self.assertEqual((Path(td)/"local-wake.state").read_text(),"closed\n")
            self.assertEqual((Path(td)/"ui.state").read_text(),"idle\n")
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        names=[e["event"] for e in events]
        self.assertLess(names.index("local_wake_detected"),names.index("local_prompt"))
        self.assertLess(names.index("local_prompt"),names.index("wake_listening"))
        self.assertLess(names.index("utterance_complete"),names.index("hello"))
        self.assertEqual(next(e["prompt"] for e in events if e["event"]=="local_prompt"),"wake")
        self.assertGreater(len(seen),0)
        self.assertNotIn("wake_accepted",names)

    def test_local_command_timeout_returns_to_offline_listener(self):
        env=self.wake_env("wake-silence");env["S12_TEST_LOCAL_WAKE_FRAME"]="10"
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=12,stop_on_event=("local_wake_listening",2))
        self.assertEqual(r.returncode,0,r.stderr)
        names=[json.loads(line)["event"] for line in r.stdout.splitlines()]
        self.assertIn("local_command_timeout",names)
        self.assertEqual(names.count("local_wake_listening"),2)
        self.assertNotIn("utterance_uploaded",names)
        self.assertNotIn("wake_retry",names)

    def test_local_command_window_advances_real_source_clocks_and_ignores_keyword_hits(self):
        for afe in [None,"healthy"]:
            with self.subTest(afe=afe):
                env=self.wake_env("wake-silence")
                env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_LOCAL_WAKE_EVERY="40")
                if afe:env["S12_TEST_AFE"]=afe
                r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=1.8)
                self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                names=[e["event"] for e in events]
                progress=next(e for e in events if e["event"]=="local_command_kws_progress")
                self.assertGreaterEqual(progress["direct_frames"],870)
                self.assertGreaterEqual(progress["ignored_direct_hits"],20)
                self.assertEqual(progress["beam_active"],bool(afe))
                if afe:
                    self.assertLessEqual(abs(progress["direct_frames"]-progress["beam_frames"]),1)
                    self.assertGreaterEqual(progress["ignored_beam_hits"],20)
                else:
                    self.assertEqual(progress["beam_frames"],0)
                    self.assertEqual(progress["ignored_beam_hits"],0)
                self.assertIn("local_command_timeout",names)
                self.assertNotIn("utterance_uploaded",names)
                self.assertNotIn("hello",names)
                first_timeout=names.index("local_command_timeout")
                self.assertEqual(names[:first_timeout].count("local_wake_detected"),1)
                self.assertEqual(names[:first_timeout].count("local_prompt"),1)

    def test_local_keyword_inside_accepted_command_does_not_start_another_turn(self):
        env=self.wake_env("wake-array")
        env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_LOCAL_WAKE_EVERY="40",S12_TEST_AFE="healthy")
        events,uploads=self.continuation_turn(env)
        names=[e["event"] for e in events]
        progress=next(e for e in events if e["event"]=="local_command_kws_progress")
        self.assertGreater(progress["ignored_direct_hits"],0)
        self.assertGreater(progress["ignored_beam_hits"],0)
        self.assertEqual(names.count("local_wake_detected"),1)
        self.assertEqual(names.count("local_prompt"),1)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertEqual(len(uploads),1)

    def test_local_idle_xrun_or_suspend_reopens_models_then_accepts_next_wake(self):
        for fault,cause in [("idle-xrun-once","xrun"),("idle-suspend-once","suspended")]:
            with self.subTest(fault=fault):
                env=self.wake_env("wake-array")
                env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_AFE="healthy",S12_TEST_CAPTURE_FAULT=fault)
                events,uploads=self.continuation_turn(env)
                names=[e["event"] for e in events]
                recovery=next(e for e in events if e["event"]=="local_capture_recovering")
                self.assertEqual(recovery["retry_ms"],250)
                self.assertTrue(recovery["models_reset"])
                self.assertFalse(recovery["old_audio_retried"])
                self.assertEqual(next(e["cause"] for e in events if e["event"]=="capture_discontinuity"),cause)
                self.assertLess(names.index("local_capture_recovering"),names.index("local_wake_detected"))
                # Fresh model epoch must start from its own frame10, rather
                # than carry a candidate across the lost microphone segment.
                self.assertEqual(next(e["input_frame"] for e in events if e["event"]=="local_wake_detected"),10)
                self.assertEqual(names.count("local_wake_detected"),1)
                self.assertEqual(names.count("local_prompt"),1)
                self.assertEqual(len(uploads),1)

    def test_local_command_xrun_discards_damaged_turn_and_next_wake_still_uploads(self):
        env=self.wake_env("wake-array")
        env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_AFE="healthy",S12_TEST_CAPTURE_FAULT="command-xrun-once")
        events,uploads=self.continuation_turn(env)
        names=[e["event"] for e in events]
        fault=names.index("capture_discontinuity")
        self.assertIn("speech_detected",names[:fault])
        self.assertNotIn("utterance_complete",names[:fault])
        self.assertNotIn("hello",names[:fault])
        self.assertEqual(names.count("local_wake_detected"),2)
        self.assertEqual(names.count("local_prompt"),2)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertEqual(len(uploads),1)

    def test_local_same_sentence_xrun_discards_preroll_then_next_sentence_is_complete(self):
        base=self.continuation_env(S12_TEST_AFE="healthy")
        _,expected=self.continuation_turn(base)
        env=dict(base,S12_TEST_CAPTURE_FAULT="sentence-xrun-once")
        events,uploads=self.continuation_turn(env)
        names=[e["event"] for e in events]
        fault=names.index("capture_discontinuity")
        self.assertIn("local_continuation_started",names[:fault])
        self.assertNotIn("local_prompt",names[:fault])
        self.assertNotIn("hello",names[:fault])
        self.assertEqual(names.count("local_wake_detected"),2)
        self.assertEqual(names.count("local_continuation_started"),2)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertEqual(uploads,expected)

    def test_local_repeated_xruns_use_saturated_slow_retry_without_stopping(self):
        env=self.wake_env("wake-silence")
        env.update(S12_TEST_CAPTURE_FAULT="xrun-repeat",S12_TEST_CAPTURE_RETRY_FAST="1",S12_TEST_AFE="healthy")
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=1.4)
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        recover=[e for e in events if e["event"]=="local_capture_recovering"]
        self.assertGreaterEqual(len(recover),9)
        self.assertLess(len(recover),20)
        self.assertEqual([e["retry_ms"] for e in recover[:8]],[250,500,1000,2000,4000,8000,16000,30000])
        self.assertTrue(all(e["retry_ms"]==30000 and e["consecutive_failures"]==9 for e in recover[8:]))
        self.assertTrue(events[-1]["interrupted"])
        self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_local_retry_real_delay_is_interruptible_and_non_xrun_io_stays_fatal(self):
        env=self.wake_env("wake-silence");env["S12_TEST_CAPTURE_FAULT"]="xrun-repeat"
        started=time.monotonic()
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=.95)
        self.assertLess(time.monotonic()-started,1.6)
        self.assertEqual(r.returncode,0,r.stderr)
        recover=[json.loads(line) for line in r.stdout.splitlines() if b'local_capture_recovering' in line]
        self.assertEqual([e["retry_ms"] for e in recover],[250,500,1000])
        r=self.invoke(args=["--local-wake"],env_change=dict(env,S12_TEST_CAPTURE_FAULT="io"))
        self.assertNotEqual(r.returncode,0)
        self.assertNotIn(b'local_capture_recovering',r.stdout)
        self.assertIn(b'ALSA capture transfer failed',r.stderr)

    def test_local_retry_counter_resets_after_real_five_second_capture(self):
        env=self.wake_env("wake-silence")
        env.update(S12_TEST_CAPTURE_FAULT="xrun-two-then-stable",S12_TEST_CAPTURE_RETRY_FAST="1")
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=1.2)
        self.assertEqual(r.returncode,0,r.stderr)
        recover=[json.loads(line) for line in r.stdout.splitlines() if b'local_capture_recovering' in line]
        self.assertEqual([e["consecutive_failures"] for e in recover],[1,2,1])
        self.assertEqual([e["retry_ms"] for e in recover],[250,500,250])
        self.assertGreaterEqual(recover[-1]["direct_frames_discarded"],500)

    def test_local_recovery_mute_does_not_mark_unloaded_models_as_healthy(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);control=root/"panel.control";control.write_text("v1 0 50 0 0 0\n");control.chmod(0o600)
            seen=[]
            def mute():
                until=time.monotonic()+2;state=root/"local-wake.state"
                while time.monotonic()<until:
                    if state.exists() and state.read_text()=="recovering\n":
                        new=root/"new";new.write_text("v1 1 50 1 1 0\n");new.chmod(0o600);new.replace(control);break
                    time.sleep(.005)
                while time.monotonic()<until:
                    ui=root/"ui.state"
                    if ui.exists() and ui.read_text()=="muted\n":seen.append(state.read_text());return
                    time.sleep(.005)
            worker=threading.Thread(target=mute);worker.start()
            env=self.wake_env("wake-silence");env.update(S12_TEST_CAPTURE_FAULT="xrun-repeat",S12_TEST_PANEL_DIR=td)
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.7)
            worker.join(1)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(seen,["recovering\n"])
            self.assertEqual(r.stdout.count(b'"event":"local_capture_recovering"'),1)
            self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_local_recovery_ota_ack_does_not_report_unloaded_models_as_healthy_paused(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);ota=root/"ota";ota.mkdir(mode=0o700)
            request=ota/"auto-apply.request";ack=ota/"auto-apply.idle";seen=[]
            def producer():
                until=time.monotonic()+2
                while time.monotonic()<until:
                    state=root/"local-wake.state"
                    if state.exists() and state.read_text()=="recovering\n":break
                    time.sleep(.005)
                request.write_text("c"*48+"\n");request.chmod(0o600)
                while time.monotonic()<until:
                    if ack.exists():
                        seen.append((ack.read_text(),state.read_text()))
                        request.unlink();return
                    time.sleep(.005)
            worker=threading.Thread(target=producer);worker.start()
            env=self.wake_env("wake-silence")
            env.update(S12_TEST_CAPTURE_FAULT="xrun-repeat",S12_TEST_PANEL_DIR=td,S12_TEST_OTA_DIR=str(ota))
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.8)
            worker.join(1)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(seen,[("c"*48+"\n","recovering\n")])
            self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_local_network_failure_plays_prompt_then_relistens_without_backoff(self):
        for connected,prompt in [(False,"wifi"),(True,"cloud")]:
            with self.subTest(connected=connected),tempfile.TemporaryDirectory(dir=self.root) as td:
                if connected:(Path(td)/"network.state").write_text("ready\n")
                env=self.wake_env();env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_PANEL_DIR=td)
                r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=.55)
                self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                prompts=[e["prompt"] for e in events if e["event"]=="local_prompt"]
                self.assertEqual(prompts,["wake",prompt])
                resume=next(e for e in events if e["event"]=="local_wake_resuming")
                self.assertEqual(resume["backoff_seconds"],0)
                self.assertFalse(resume["old_audio_retried"])
                self.assertEqual(sum(e["event"]=="local_wake_listening" for e in events),2)

    def test_local_bf_reframes_and_weak_or_clipped_output_falls_back_before_lock(self):
        # A fresh AFE epoch starts with an unverified noise distribution; the
        # first immediate onset is conservatively selected from a healthy mic.
        for mode,expected in [("healthy","six_channel_selection"),("weak","six_channel_selection"),("clipped","six_channel_selection")]:
            with self.subTest(mode=mode):
                def server(p):
                    self.wake_upload(p,source="manual")
                    p.sock.sendall(frame(1,{"type":"tts","state":"start"})+
                                   frame(1,{"type":"tts","state":"stop"}))
                    self.assertEqual(p.receive()[0],8)
                env=self.wake_env("wake-array");env.update(S12_TEST_LOCAL_WAKE_FRAME="70",S12_TEST_AFE=mode)
                r=self.invoke(server,["--local-wake","--wake-rounds","1"],env_change=env)
                self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                source=next(e for e in events if e["event"]=="local_command_frontend")
                self.assertEqual(source["source"],expected)
                self.assertTrue(source["locked"])

    def test_local_fallback_preserves_first_immediate_command_packet(self):
        packets=[]
        def server(p):
            data,_=self.wake_upload(p,source="manual");packets.extend(data)
            p.sock.sendall(frame(1,{"type":"tts","state":"start"})+frame(1,{"type":"tts","state":"stop"}))
            self.assertEqual(p.receive()[0],8)
        env=self.wake_env("wake-array")
        env.update(S12_TEST_LOCAL_WAKE_FRAME="10",S12_TEST_AFE="weak",S12_TEST_COMMAND_IMMEDIATE="1")
        r=self.invoke(server,["--local-wake","--wake-rounds","1"],env_change=env)
        self.assertEqual(r.returncode,0,r.stderr)
        audio=decode_uplink(packets)
        # Only the first capture packet has 443 Hz. Opus lookahead delays its
        # beginning; this region remains in that packet after decoding. If BF
        # warmup discarded the first raw60ms, this would contain only997Hz.
        prefix=audio[800:1120]
        def tone(hz):
            real=sum(x*math.cos(2*math.pi*hz*i/16000) for i,x in enumerate(prefix))
            imag=sum(x*math.sin(2*math.pi*hz*i/16000) for i,x in enumerate(prefix))
            return real*real+imag*imag
        self.assertGreater(tone(443),tone(997)*10)

    def test_local_cue_echo_and_independent_bf_ambient_do_not_upload(self):
        # Captured from the real quiet-only replay: raw cue tail falls across
        # three packets, while later BF noise is much louder than the raw mic.
        # Exercise both an established independent prior and a cold first wake.
        for hit in [100,10]:
            with self.subTest(hit=hit):
                env=self.wake_env("wake-cue-trace")
                env.update(S12_TEST_LOCAL_WAKE_FRAME=str(hit),S12_TEST_AFE="cue-trace")
                r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=12,stop_on_event="local_command_timeout")
                self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                self.assertIn("local_command_timeout",[e["event"] for e in events])
                self.assertNotIn("speech_detected",[e["event"] for e in events])
                self.assertNotIn("utterance_uploaded",[e["event"] for e in events])
                prior=next(e for e in events if e["event"]=="local_bf_noise_prior")
                # Reopening creates a new AFE instance. Never transplant an
                # old instance's learned distribution into its command epoch.
                self.assertFalse(prior["verified"])
                self.assertEqual(prior["noise_rms"],0)
                self.assertEqual(prior["cold_guard_ms"],180)

    def test_afe_availability_changes_do_not_mix_raw_and_bf_noise(self):
        for mode,hit in [("recover-available",100),("fail-after-prior",100)]:
            with self.subTest(mode=mode):
                env=self.wake_env("wake-cue-trace")
                env.update(S12_TEST_LOCAL_WAKE_FRAME=str(hit),S12_TEST_AFE=mode)
                r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=4)
                self.assertEqual(r.returncode,0,r.stderr)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                prior=next(e for e in events if e["event"]=="local_bf_noise_prior")
                self.assertFalse(prior["verified"])
                self.assertEqual(prior["noise_rms"],0)
                self.assertEqual(prior["cold_guard_ms"],180)
                self.assertNotIn("speech_detected",[e["event"] for e in events])
                self.assertIn("local_command_timeout",[e["event"] for e in events])

    def test_verified_bf_prior_keeps_weak_commands_after_cue_settling(self):
        # The raw waveform remains below its VAD threshold, while the BF
        # supplies a 180/240/300ms command after500ms over its separately
        # learned35RMS ambient. It must freeze its floor, not learn the voice.
        for duration in [180,240,300]:
            with self.subTest(duration=duration):
                env=self.wake_env("wake-weak-beam")
                env.update(S12_TEST_LOCAL_WAKE_FRAME="100",S12_TEST_AFE="ambient-weak",
                           S12_TEST_COMMAND_IMMEDIATE="1",S12_TEST_COMMAND_DURATION_MS=str(duration),
                           S12_TEST_BF_COMMAND_BEGIN_MS="500")
                events,uploads=self.continuation_turn(env)
                source=next(e for e in events if e["event"]=="local_command_frontend")
                self.assertEqual(source["source"],"original_beamformer")
                self.assertEqual(source["cold_guard_ms"],180)
                self.assertLess(source["noise_rms"],40)
                onset=next(e for e in events if e["event"]=="speech_detected")
                self.assertGreater(onset["frame_rms"],150)
                self.assertLess(onset["threshold_rms"],140)
                samples=decode_uplink(uploads[0])
                queue_start_ms=max(0,onset["onset_block"]*60-onset["preroll_ms"])
                # The fixture injects the weak harmonic at500ms of its fresh
                # AFE epoch. Map that into uploaded PCM instead of measuring a
                # fixed281..406ms noise-only window from an older preroll.
                start=round((500-queue_start_ms+40)*16)
                self.assertGreaterEqual(start,0)
                part=samples[start:start+1600]
                self.assertEqual(len(part),1600)
                def energy(hz):
                    return abs(sum(x*complex(math.cos(2*math.pi*hz*i/16000),math.sin(2*math.pi*hz*i/16000)) for i,x in enumerate(part)))**2
                self.assertGreater(energy(997),energy(311)*5)

    def test_immediate_extremely_short_weak_word_guard_is_explicit(self):
        # An accepted conservative limit: energy alone cannot separate an
        # immediate tiny word from a changed AFE/cue tail. This must not be
        # advertised as lossless180ms command support.
        for duration in [180,240,300]:
            env=self.wake_env("wake-weak-beam")
            env.update(S12_TEST_LOCAL_WAKE_FRAME="100",S12_TEST_AFE="ambient-weak",
                       S12_TEST_COMMAND_IMMEDIATE="1",S12_TEST_COMMAND_DURATION_MS=str(duration))
            r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=4)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn(b'local_command_timeout',r.stdout)
            self.assertNotIn(b'speech_detected',r.stdout)
            self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_new_bf_epoch_recalibrates_ambient_instead_of_reusing_prior(self):
        env=self.wake_env("wake-weak-beam")
        env.update(S12_TEST_LOCAL_WAKE_FRAME="400",S12_TEST_AFE="startup-ambient")
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=4)
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        ready=next(e for e in events if e["event"]=="local_bf_noise_ready")
        prior=next(e for e in events if e["event"]=="local_bf_noise_prior")
        self.assertGreater(ready["noise_rms"],180)
        self.assertFalse(prior["verified"]);self.assertEqual(prior["noise_rms"],0)
        self.assertIn(b'local_command_timeout',r.stdout)
        self.assertNotIn(b'speech_detected',r.stdout)
        self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_uncalibrated_bf_does_not_delay_independent_raw_onset(self):
        env=self.wake_env("wake-array")
        env.update(S12_TEST_LOCAL_WAKE_FRAME="100",S12_TEST_AFE="source-switch",S12_TEST_COMMAND_IMMEDIATE="1")
        events,_=self.continuation_turn(env)
        source=next(e for e in events if e["event"]=="local_command_frontend")
        onset=next(e for e in events if e["event"]=="speech_detected")
        self.assertEqual(source["source"],"six_channel_selection")
        # A fresh unverified BF epoch now starts from the selected microphone;
        # it cannot borrow the hypothetical BF packet4/5 onset. Three genuine
        # raw frames complete onblock6 under this fixture's current contract.
        self.assertEqual(onset["onset_block"],6)

    def test_local_mute_during_capture_wait_does_not_exit_listener(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);control=root/"panel.control";control.write_text("v1 0 50 0 0 0\n");control.chmod(0o600)
            def mute():
                time.sleep(.15);new=root/"new";new.write_text("v1 1 50 1 1 0\n");new.chmod(0o600);new.replace(control)
            worker=threading.Thread(target=mute);worker.start()
            env=self.wake_env();env.update(S12_TEST_PANEL_DIR=td,S12_TEST_CAPTURE_FAULT="timeout")
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.45)
            worker.join(1)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertIn(b'"interrupted":true',r.stdout)
            self.assertNotIn(b'utterance_uploaded',r.stdout)
            self.assertEqual((root/"ui.state").read_text(),"muted\n")

    @staticmethod
    def continuation_env(**values):
        env={"S12_TEST_CAPTURE_PROFILE":"local-sentence","S12_TEST_CAPTURE_FAST":"1",
             "S12_TEST_LOCAL_CONTINUATION":"1","S12_TEST_LOCAL_WAKE_FRAME":"70",
             "S12_TEST_LOCAL_WAKE_BEGIN_FRAME":"30","S12_TEST_LOCAL_WAKE_END_FRAME":"50",
             "S12_TEST_CONTINUATION_END_MS":"1200"}
        env.update({key:str(value) for key,value in values.items()});return env

    def continuation_turn(self,env,connections=1,accept_timeout=12):
        uploads=[]
        def server(p):
            packets,_=self.wake_upload(p,source="manual");uploads.append(packets)
            p.sock.sendall(frame(1,{"type":"tts","state":"start"})+frame(1,{"type":"tts","state":"stop"}))
            self.assertEqual(p.receive()[0],8)
        r=self.invoke(server,["--local-wake","--wake-rounds",str(connections)],env_change=env,mock_connections=connections,mock_accept_timeout=accept_timeout)
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        return events,uploads

    def test_capture_reader_absorbs_inference_stall_without_losing_or_repeating_pcm(self):
        env=self.continuation_env(S12_TEST_CAPTURE_FAST=0,S12_TEST_AFE="healthy")
        _,expected=self.continuation_turn(env)
        env.update(S12_TEST_KWS_STALL_FRAME="70",S12_TEST_KWS_STALL_MS="400")
        events,uploads=self.continuation_turn(env)
        closed=next(e for e in events if e["event"]=="capture_reader_closed")
        self.assertTrue(closed["reader_joined"])
        self.assertGreaterEqual(closed["fifo_peak_frames"],14400)
        self.assertLess(closed["fifo_peak_frames"],96000)
        self.assertGreaterEqual(closed["source_frames"],closed["consumed_frames"]+closed["queued_frames_discarded"])
        self.assertEqual(uploads,expected)
        self.assertFalse(any(e["event"]=="capture_discontinuity" for e in events))
        names=[e["event"] for e in events]
        self.assertLess(names.index("capture_reader_closed"),names.index("utterance_complete"))
        self.assertLess(names.index("capture_reader_closed"),names.index("local_prompt"))

    def test_capture_reader_three_second_fifo_absorbs_bounded_two_second_stall(self):
        env=self.continuation_env(S12_TEST_CAPTURE_FAST=0,S12_TEST_AFE="healthy")
        _,expected=self.continuation_turn(env)
        env.update(S12_TEST_KWS_STALL_FRAME="70",S12_TEST_KWS_STALL_MS="2200")
        events,uploads=self.continuation_turn(env,accept_timeout=8)
        closed=next(e for e in events if e["event"]=="capture_reader_closed")
        self.assertGreater(closed["fifo_peak_frames"],96000)
        self.assertLess(closed["fifo_peak_frames"],144000)
        self.assertEqual(uploads,expected)
        self.assertFalse(any(e["event"]=="capture_discontinuity" for e in events))
        self.assertTrue(closed["reader_joined"])

    def test_capture_reader_overflow_discards_whole_turn_then_next_epoch_works(self):
        env=self.continuation_env(S12_TEST_CAPTURE_FAST=0,S12_TEST_AFE="healthy",
                                  S12_TEST_KWS_STALL_FRAME=70,S12_TEST_KWS_STALL_MS=3300)
        events,uploads=self.continuation_turn(env,accept_timeout=8)
        names=[e["event"] for e in events]
        faults=[e for e in events if e["event"]=="capture_discontinuity"]
        self.assertEqual([e["cause"] for e in faults],["fifo_overflow"])
        fault=names.index("capture_discontinuity")
        self.assertNotIn("utterance_complete",names[:fault])
        self.assertNotIn("local_prompt",names[:fault])
        self.assertNotIn("hello",names[:fault])
        self.assertEqual(names.count("local_capture_recovering"),1)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertEqual(len(uploads),1)
        self.assertEqual([e["input_frame"] for e in events if e["event"]=="local_wake_detected"],[70,70])
        self.assertTrue(all(e["reader_joined"] for e in events if e["event"]=="capture_reader_closed"))

    def test_capture_reader_terminal_error_after_last_packet_prevents_false_success(self):
        env=self.continuation_env(S12_TEST_AFE="healthy",S12_TEST_CAPTURE_LATE_ERROR=1)
        events,uploads=self.continuation_turn(env)
        names=[e["event"] for e in events]
        self.assertEqual(names.count("local_wake_detected"),2)
        self.assertEqual(names.count("utterance_complete"),1)
        self.assertEqual(names.count("local_prompt"),1)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertEqual(len(uploads),1)
        self.assertLess(names.index("capture_discontinuity"),names.index("utterance_complete"))
        self.assertEqual(next(e["stage"] for e in events if e["event"]=="capture_discontinuity"),"reader_close")

    def test_capture_reader_join_remains_responsive_to_term_during_inference_peak(self):
        env=self.continuation_env(S12_TEST_CAPTURE_FAST=0,S12_TEST_AFE="healthy",
                                  S12_TEST_KWS_STALL_FRAME=70,S12_TEST_KWS_STALL_MS=400)
        started=time.monotonic()
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=.85)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertLess(time.monotonic()-started,1.5)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        closed=[e for e in events if e["event"]=="capture_reader_closed"]
        self.assertEqual(len(closed),1)
        self.assertTrue(closed[0]["reader_joined"])
        self.assertNotIn(b'local_prompt',r.stdout)
        self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_capture_reader_joins_before_mute_with_inference_backlog(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            p=Path(td);control=p/"panel.control";control.write_text("v1 0 50 0 0 0\n");control.chmod(0o600)
            def mute():
                time.sleep(.85);new=p/"new";new.write_text("v1 1 50 1 1 0\n");new.chmod(0o600);new.replace(control)
            worker=threading.Thread(target=mute);worker.start()
            env=self.continuation_env(S12_TEST_CAPTURE_FAST=0,S12_TEST_AFE="healthy",S12_TEST_PANEL_DIR=td,
                                      S12_TEST_KWS_STALL_FRAME=70,S12_TEST_KWS_STALL_MS=400)
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=1.4)
            worker.join(1)
            self.assertEqual(r.returncode,0,r.stderr)
            events=[json.loads(line) for line in r.stdout.splitlines()]
            self.assertTrue(next(e["reader_joined"] for e in events if e["event"]=="capture_reader_closed"))
            self.assertEqual((p/"ui.state").read_text(),"muted\n")
            self.assertTrue(all(e.get('prompt')=='button' for e in events if e['event']=='local_prompt'))
            for i,event in enumerate(events):
                if event['event']=='local_prompt':self.assertIn('local_capture_closed',[e['event'] for e in events[:i]])
            self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_existing_panel_manual_counter_is_consumed_as_startup_baseline(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            p=Path(td);control=p/"panel.control";control.write_text("v1 9 50 0 5 2\n");control.chmod(0o600)
            env=self.wake_env("wake-array");env.update(S12_TEST_PANEL_DIR=td,S12_TEST_AFE="healthy")
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.3)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'local_prompt',r.stdout)
            self.assertNotIn(b'utterance_uploaded',r.stdout)
            control.write_text("v1 9 50 1 5 2\n")
            r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=.2)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'capture_reader_started',r.stdout)
            self.assertEqual((p/"ui.state").read_text(),"muted\n")

    def test_local_same_sentence_keeps_keyword_tail_and_never_plays_during_capture(self):
        # Each 60ms sub-frame can carry the accepted hit; its unprocessed tail
        # must still enter the same detector's ring before deciding a turn.
        for hit in range(67,73):
            with self.subTest(hit=hit):
                events,uploads=self.continuation_turn(self.continuation_env(S12_TEST_LOCAL_WAKE_FRAME=hit))
                names=[e["event"] for e in events]
                self.assertEqual(names.count("local_wake_listening"),1)
                self.assertNotIn("wake_listening",names)
                self.assertEqual(names.count("local_continuation_started"),1)
                self.assertLess(names.index("utterance_complete"),names.index("local_capture_closed"))
                self.assertLess(names.index("local_capture_closed"),names.index("local_prompt"))
                self.assertLess(names.index("local_prompt"),names.index("hello"))
                complete=next(e for e in events if e["event"]=="utterance_complete")
                self.assertTrue(complete["same_sentence"])
                self.assertLess(complete["padding_samples"],960)
                audio=decode_uplink(uploads[0])
                def energy(hz,part):
                    real=sum(x*math.cos(2*math.pi*hz*i/16000) for i,x in enumerate(part))
                    imag=sum(x*math.sin(2*math.pi*hz*i/16000) for i,x in enumerate(part))
                    return real*real+imag*imag
                self.assertGreater(energy(443,audio[2000:2800]),energy(997,audio[2000:2800])*10)
                self.assertGreater(energy(997,audio[7000:9000]),energy(443,audio[7000:9000])*10)
                # 1301Hz belonged only to the earlier idle sentence, outside
                # the precise confirmed keyword's authorized 80ms preroll.
                self.assertLess(energy(1301,audio),energy(443,audio)*.01)

    def test_local_bf_delayed_sentence_uses_its_own_ring_clock(self):
        env=self.continuation_env(S12_TEST_AFE="healthy",S12_TEST_AFE_DELAY_FRAMES=5,
                                  S12_TEST_LOCAL_WAKE_SOURCE="beam",S12_TEST_LOCAL_WAKE_FRAME=105,
                                  S12_TEST_LOCAL_WAKE_BEGIN_FRAME=65,S12_TEST_LOCAL_WAKE_END_FRAME=85,
                                  S12_TEST_SENTENCE_BEGIN_MS=600,S12_TEST_SENTENCE_END_MS=800,
                                  S12_TEST_CONTINUATION_START_MS=800,S12_TEST_CONTINUATION_END_MS=1500)
        events,uploads=self.continuation_turn(env)
        start=next(e for e in events if e["event"]=="local_continuation_started")
        self.assertEqual(start["source"],"original_beamformer")
        self.assertEqual(start["first_frame"],57)
        self.assertFalse(start["capture_reopened"])
        audio=decode_uplink(uploads[0]);prefix=audio[2000:2800]
        def energy(hz):
            return abs(sum(x*complex(math.cos(2*math.pi*hz*i/16000),math.sin(2*math.pi*hz*i/16000)) for i,x in enumerate(prefix)))**2
        self.assertGreater(energy(443),energy(997)*10)

    def test_local_short_pre_hit_command_ambiguous_with_word_tail_falls_back(self):
        # Keyword ends500ms; command is530..650ms, but the detector emits at
        # 700ms. Real-model evidence shows the public word end can precede its
        # acoustic tail270ms. This short case is intentionally ambiguous;
        # request a fresh command rather than upload an ordinary wake-only.
        env=self.continuation_env(S12_TEST_CONTINUATION_START_MS=530,S12_TEST_CONTINUATION_END_MS=650)
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=12,stop_on_event="local_command_timeout")
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn(b'local_continuation_started',r.stdout)
        self.assertNotIn(b'utterance_uploaded',r.stdout)
        self.assertIn(b'"prompt":"wake"',r.stdout)

    def test_local_keyword_only_or_short_noise_never_uploads(self):
        for end in [500,570]:
            with self.subTest(end=end):
                env=self.continuation_env(S12_TEST_CONTINUATION_START_MS=520,S12_TEST_CONTINUATION_END_MS=end)
                r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=12,stop_on_event="local_command_timeout")
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertIn(b'"prompt":"wake"',r.stdout)
                self.assertNotIn(b'local_continuation_started',r.stdout)
                self.assertNotIn(b'utterance_uploaded',r.stdout)
                self.assertNotIn(b'"hello"',r.stdout)

    def test_local_model_end_before_spoken_keyword_tail_does_not_upload_wake_only(self):
        # Match the real SDK's observed270ms alignment/energy discrepancy:
        # model end500ms, spokenword tail770ms, finalhit760ms. The discarded
        # end+20ms/80ms policy would falsely upload the final 学 in this case.
        env=self.continuation_env(S12_TEST_SENTENCE_END_MS=770,S12_TEST_LOCAL_WAKE_FRAME=76,
                                  S12_TEST_CONTINUATION_START_MS=10000,S12_TEST_CONTINUATION_END_MS=0)
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=12,stop_on_event="local_command_timeout")
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn(b'"prompt":"wake"',r.stdout)
        self.assertNotIn(b'local_continuation_started',r.stdout)
        self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_local_invalid_or_expired_timing_falls_back_without_idle_audio(self):
        cases=[{"S12_TEST_LOCAL_TIMING_INVALID":"1"},
               {"S12_TEST_LOCAL_WAKE_END_FRAME":"18446744073709551615"},
               {"S12_TEST_LOCAL_WAKE_FRAME":"370","S12_TEST_LOCAL_WAKE_BEGIN_FRAME":"10","S12_TEST_LOCAL_WAKE_END_FRAME":"350"}]
        for change in cases:
            with self.subTest(change=change):
                r=self.invoke(args=["--local-wake"],env_change=self.continuation_env(**change),interrupt_after=4)
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertIn(b'local_timing_fallback',r.stdout)
                self.assertNotIn(b'local_continuation_started',r.stdout)
                self.assertNotIn(b'utterance_uploaded',r.stdout)

    def test_local_prefix_expiring_during_guard_falls_back_without_exiting(self):
        env=self.continuation_env(S12_TEST_CAPTURE_PROFILE="wake-max",S12_TEST_LOCAL_WAKE_FRAME=310,
                                  S12_TEST_LOCAL_WAKE_BEGIN_FRAME=1,S12_TEST_LOCAL_WAKE_END_FRAME=300)
        r=self.invoke(args=["--local-wake"],env_change=env,interrupt_after=1.5)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn(b'"reason":"preroll_expired"',r.stdout)
        self.assertNotIn(b'local continuation preroll unavailable',r.stderr)
        self.assertIn(b'"interrupted":true',r.stdout)

    def test_local_same_sentence_is_bounded_to_twelve_seconds(self):
        env=self.continuation_env(S12_TEST_CONTINUATION_END_MS=20000)
        events,uploads=self.continuation_turn(env)
        complete=next(e for e in events if e["event"]=="utterance_complete")
        self.assertEqual(len(uploads[0]),200)
        self.assertEqual(complete["duration_ms"],12000)
        self.assertTrue(complete["max_duration_reached"])

    def test_local_second_sentence_uses_cumulative_context_frame_ids(self):
        events,uploads=self.continuation_turn(self.continuation_env(S12_TEST_LOCAL_WAKE_EVERY=228),connections=2)
        hits=[e for e in events if e["event"]=="local_wake_detected"]
        starts=[e for e in events if e["event"]=="local_continuation_started"]
        self.assertEqual([e["input_frame"] for e in hits],[70,298])
        self.assertEqual([e["end_frame"] for e in hits],[50,278])
        self.assertEqual([e["first_frame"] for e in starts],[22,250])
        self.assertEqual(uploads[0],uploads[1])
        self.assertFalse(any(e["event"]=="local_timing_fallback" for e in events))

    def test_local_wake_only_timeout_then_sentence_preserves_both_context_clocks(self):
        env=self.continuation_env(S12_TEST_AFE="healthy",S12_TEST_CONTINUATION_AFTER_TIMEOUT=1,
                                  S12_TEST_LOCAL_WAKE_EVERY=936)
        events,uploads=self.continuation_turn(env)
        names=[e["event"] for e in events]
        hits=[e for e in events if e["event"]=="local_wake_detected"]
        progress=next(e for e in events if e["event"]=="local_command_kws_progress")
        self.assertEqual([e["input_frame"] for e in hits],[70,1006])
        self.assertEqual([e["end_frame"] for e in hits],[50,986])
        self.assertEqual(progress["direct_frames"],936)
        self.assertEqual(progress["beam_frames"],936)
        self.assertEqual(names.count("local_command_timeout"),1)
        self.assertEqual(names.count("local_continuation_started"),1)
        self.assertEqual(names.count("utterance_uploaded"),1)
        self.assertLess(names.index("local_command_timeout"),names.index("hello"))
        self.assertEqual(len(uploads),1)
        self.assertFalse(any(e["event"]=="local_timing_fallback" for e in events))

    def test_local_continuation_onset_at_grace_boundary_is_not_cut(self):
        for pause in [0,200,300,800]:
            with self.subTest(pause=pause):
                start=500+pause
                events,uploads=self.continuation_turn(self.continuation_env(
                    S12_TEST_CONTINUATION_START_MS=start,S12_TEST_CONTINUATION_END_MS=start+700))
                self.assertEqual(len(uploads),1)
                self.assertTrue(any(e["event"]=="local_continuation_started" for e in events))

    def test_local_grace_and_continuation_mute_discard_carry_without_upload(self):
        for stage in ["grace","continuation"]:
            with self.subTest(stage=stage),tempfile.TemporaryDirectory(dir=self.root) as td:
                root=Path(td);control=root/"panel.control";control.write_text("v1 0 50 0 0 0\n");control.chmod(0o600)
                muted=[]
                def mute():
                    until=time.monotonic()+2
                    while time.monotonic()<until:
                        state=root/("local-wake.state" if stage=="grace" else "ui.state")
                        if state.exists() and state.read_text()==("busy\n" if stage=="grace" else "capturing\n"):
                            new=root/"new";new.write_text("v1 1 50 1 1 0\n");new.chmod(0o600);new.replace(control);muted.append(True);return
                        time.sleep(.005)
                env=self.continuation_env(S12_TEST_CAPTURE_FAST="0",S12_TEST_PANEL_DIR=td)
                if stage=="grace":env.update(S12_TEST_CONTINUATION_START_MS="1000",S12_TEST_CONTINUATION_END_MS="1200")
                worker=threading.Thread(target=mute);worker.start()
                r=self.invoke(args=["--local-wake","--panel"],env_change=env,interrupt_after=1.1)
                worker.join(1)
                self.assertEqual(muted,[True])
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertNotIn(b'utterance_uploaded',r.stdout)
                events=[json.loads(line) for line in r.stdout.splitlines()]
                self.assertTrue(all(e.get('prompt')=='button' for e in events if e['event']=='local_prompt'))
                for i,event in enumerate(events):
                    if event['event']=='local_prompt':self.assertIn('local_capture_closed',[e['event'] for e in events[:i]])
                self.assertEqual((root/"ui.state").read_text(),"muted\n")

    def test_local_ota_waits_for_same_sentence_capture_and_network_turn_to_finish(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);ota=root/"ota";ota.mkdir(mode=0o700)
            request=ota/"auto-apply.request";ack=ota/"auto-apply.idle";seen=[]
            def producer():
                until=time.monotonic()+3
                while time.monotonic()<until:
                    state=root/"local-wake.state"
                    if state.exists() and state.read_text()=="busy\n":break
                    time.sleep(.005)
                request.write_text("d"*48+"\n");request.chmod(0o600)
                time.sleep(.15);seen.append(not ack.exists())
                while time.monotonic()<until:
                    if ack.exists():seen.append(ack.read_text());request.unlink();return
                    time.sleep(.01)
            def server(p):
                self.wake_upload(p,source="manual");self.assertFalse(ack.exists())
                p.sock.sendall(frame(1,{"type":"tts","state":"start"})+frame(1,{"type":"tts","state":"stop"}))
                self.assertEqual(p.receive()[0],8)
            worker=threading.Thread(target=producer);worker.start()
            env=self.continuation_env(S12_TEST_CAPTURE_FAST="0",S12_TEST_PANEL_DIR=td,S12_TEST_OTA_DIR=str(ota))
            r=self.invoke(server,["--local-wake","--panel"],env_change=env,interrupt_after=2.6)
            worker.join(1)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(seen,[True,"d"*48+"\n"])
            names=[json.loads(line)["event"] for line in r.stdout.splitlines()]
            self.assertLess(names.index("turn_complete"),names.index("ota_idle_enter"))
            self.assertLess(names.index("local_capture_closed"),names.index("ota_idle_enter"))

    def test_wake_error_discards_audio_backs_off_then_relistens(self):
        count = []
        def server(p):
            self.wake_upload(p)
            count.append(time.monotonic())
            p.sock.sendall(frame(1, {"type": "wake", "state": "error" if len(count) == 1 else "ignored"}))
            if len(count) == 2:
                self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "2"],
                        env_change=self.wake_env(), mock_connections=2)
        self.assertEqual(r.returncode, 0, r.stderr)
        events = [json.loads(line) for line in r.stdout.splitlines()]
        retry = [e for e in events if e["event"] == "wake_retry"]
        self.assertEqual(len(retry), 1)
        self.assertEqual(retry[0]["backoff_seconds"], 1)
        self.assertFalse(retry[0]["old_audio_retried"])
        self.assertGreaterEqual(count[1] - count[0], 1.0)
        self.assertEqual(sum(e["event"] == "utterance_complete" for e in events), 2)
        self.assertNotIn(b"turn_complete", r.stdout)

    def test_wake_at_least_100_packets_burst_upload_has_exact_stop_barrier(self):
        seen = []
        def server(p):
            packets, elapsed = self.wake_upload(p)
            seen.append((packets, elapsed))
            p.sock.sendall(frame(1, {"type": "wake", "state": "ignored"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "1"], env_change=self.wake_env("wake-long"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreaterEqual(len(seen[0][0]), 100)
        # Replaying >= 6 s of speech must not add another >= 6 s upload delay.
        self.assertLess(seen[0][1], 1.25)
        self.assertEqual(len(decode_uplink(seen[0][0])), len(seen[0][0]) * 960)

    def test_wake_maximum_12_seconds_is_bounded(self):
        seen = []
        def server(p):
            packets, _ = self.wake_upload(p)
            seen.extend(packets)
            p.sock.sendall(frame(1, {"type": "wake", "state": "ignored"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "1"], env_change=self.wake_env("wake-max"))
        self.assertEqual(r.returncode, 0, r.stderr)
        event = next(json.loads(line) for line in r.stdout.splitlines() if b'"utterance_complete"' in line)
        self.assertEqual(len(seen), 200)
        self.assertEqual(event["duration_ms"], 12000)
        self.assertTrue(event["max_duration_reached"])
        self.assertLessEqual(event["compressed_bytes"], 512 * 1024)

    def test_wake_dc_silence_and_rejected_12khz_channel_never_open_network(self):
        for profile, channel in [("wake-silence", 0), ("wake-burst", 1), ("wake-soft-noise", 0)]:
            with self.subTest(profile=profile, channel=channel):
                r = self.invoke(args=["--wake-loop", "--mic-channel", str(channel)],
                                env_change=self.wake_env(profile), interrupt_after=0.3)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn(b"wake_listening", r.stdout)
                self.assertNotIn(b'"hello"', r.stdout)
                self.assertNotIn(b"speech_detected", r.stdout)
                self.assertNotIn(b"utterance_uploaded", r.stdout)
                self.assertIn(b'"submitted_rounds":0', r.stdout)

    def test_wake_low_amplitude_45_rms_with_6_rms_noise_and_dc_is_uploaded(self):
        packets = []
        def server(p):
            data, _ = self.wake_upload(p)
            packets.extend(data)
            p.sock.sendall(frame(1, {"type": "wake", "state": "ignored"}))
            self.assertEqual(p.receive()[0], 8)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "1"], env_change=self.wake_env("wake-soft"))
        self.assertEqual(r.returncode, 0, r.stderr)
        onset = next(json.loads(line) for line in r.stdout.splitlines() if b'"speech_detected"' in line)
        self.assertGreater(onset["frame_rms"], 43)
        self.assertLess(onset["frame_rms"], 48)
        self.assertEqual(onset["threshold_rms"], 30)
        self.assertEqual(onset["preroll_ms"], 240)
        self.assertGreater(len(packets), 20)
        self.assertEqual(len(decode_uplink(packets)), len(packets) * 960)

    def test_auto_mic_uses_clean_channel_and_preserves_uniform_preroll(self):
        packets=[]
        def server(p):
            data,_=self.wake_upload(p);packets.extend(data)
            p.sock.sendall(frame(1,{"type":"wake","state":"ignored"}))
            self.assertEqual(p.receive()[0],8)
        r=self.invoke(server,["--wake-loop","--wake-rounds","1","--mic-channel","auto"],
                      env_change=self.wake_env("wake-array"))
        self.assertEqual(r.returncode,0,r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        locked=[e for e in events if e["event"]=="mic_array" and e["phase"]=="utterance_lock"]
        self.assertEqual(len(locked),1)
        self.assertIn(locked[0]["selected"],[2,4])
        self.assertTrue(locked[0]["locked"])
        health={e["channel"]:e for e in locked[0]["channels"]}
        self.assertFalse(health[1]["healthy"])
        self.assertFalse(health[5]["healthy"])
        onset=next(e for e in events if e["event"]=="speech_detected")
        self.assertEqual(onset["preroll_ms"],240)
        self.assertLess(onset["frame_rms"],520)
        self.assertGreater(onset["frame_rms"],450)
        audio=decode_uplink(packets)
        # The full 240 ms preroll belongs to the locked channel, including
        # samples before selection changed. Loud stationary mic0 cannot leak.
        # Opus's initial high-pass/DC settling transient is included here;
        # even with that transient it must stay far below noisy mic0's RMS.
        self.assertLess(math.sqrt(sum(x*x for x in audio[:600])/600),1200)
        voiced=audio[4000:8000]
        self.assertGreater(math.sqrt(sum(x*x for x in voiced)/len(voiced)),300)

    def test_ota_idle_ack_pauses_capture_then_resumes_without_network(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            root=Path(td);ota=root/"ota";ota.mkdir(mode=0o700)
            panel=root/"panel";panel.mkdir(mode=0o700)
            request=ota/"auto-apply.request";request.write_text("a"*48+"\n");request.chmod(0o600)
            observations=[]
            def remove_request():
                deadline=time.monotonic()+2
                while time.monotonic()<deadline:
                    ack=ota/"auto-apply.idle"
                    if ack.exists():
                        observations.append((ack.read_text(),ack.stat().st_mode&0o777,(panel/"ui.state").read_text()))
                        request.unlink();return
                    time.sleep(.01)
            watcher=threading.Thread(target=remove_request);watcher.start()
            env=self.wake_env("wake-silence");env.update(S12_TEST_OTA_DIR=str(ota),S12_TEST_PANEL_DIR=str(panel))
            r=self.invoke(args=["--wake-loop","--panel"],env_change=env,interrupt_after=.5)
            watcher.join(3)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(observations,[("a"*48+"\n",0o600,"idle\n")])
            events=[json.loads(line)["event"] for line in r.stdout.splitlines()]
            self.assertLess(events.index("ota_idle_enter"),events.index("wake_listening"))
            self.assertLess(events.index("ota_idle_exit"),events.index("wake_listening"))
            self.assertNotIn("utterance_uploaded",events)
            self.assertFalse((ota/"auto-apply.idle").exists())

    def test_ota_idle_rejects_nonprivate_malformed_and_symlink_requests(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            ota=Path(td);ota.chmod(0o700)
            request=ota/"auto-apply.request"
            for data,mode in [("a"*48+"\n",0o644),("A"*48+"\n",0o600),("a"*47+"\n",0o600)]:
                request.write_text(data);request.chmod(mode)
                env=self.wake_env("wake-silence");env["S12_TEST_OTA_DIR"]=td
                r=self.invoke(args=["--wake-loop"],env_change=env,interrupt_after=.2)
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertNotIn(b'"ota_idle_enter"',r.stdout)
                self.assertIn(b'"wake_listening"',r.stdout)
                self.assertFalse((ota/"auto-apply.idle").exists())
            request.unlink();secret=ota/"other";secret.write_text("a"*48+"\n");secret.chmod(0o600)
            request.symlink_to(secret)
            env=self.wake_env("wake-silence");env["S12_TEST_OTA_DIR"]=td
            r=self.invoke(args=["--wake-loop"],env_change=env,interrupt_after=.2)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'"ota_idle_enter"',r.stdout)
            self.assertFalse((ota/"auto-apply.idle").exists())

    def test_ota_request_during_idle_capture_closes_then_resumes(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            ota=Path(td);ota.chmod(0o700);seen=[]
            def producer():
                time.sleep(.08)
                request=ota/"auto-apply.request";request.write_text("c"*48+"\n");request.chmod(0o600)
                deadline=time.monotonic()+2
                while time.monotonic()<deadline:
                    ack=ota/"auto-apply.idle"
                    if ack.exists():
                        seen.append(ack.read_text());request.unlink();return
                    time.sleep(.01)
            worker=threading.Thread(target=producer);worker.start()
            env=self.wake_env("wake-silence");env["S12_TEST_OTA_DIR"]=td
            r=self.invoke(args=["--wake-loop"],env_change=env,interrupt_after=.6)
            worker.join(3)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(seen,["c"*48+"\n"])
            events=[json.loads(line) for line in r.stdout.splitlines()]
            entered=next(e for e in events if e["event"]=="ota_idle_enter")
            self.assertTrue(entered["capture_closed"])
            self.assertEqual(sum(e["event"]=="wake_listening" for e in events),2)
            self.assertFalse(any(e["event"]=="wake_retry" for e in events))
            self.assertFalse(any(e["event"]=="utterance_uploaded" for e in events))

    def test_ota_request_during_network_turn_is_not_acknowledged_early(self):
        with tempfile.TemporaryDirectory(dir=self.root) as td:
            ota=Path(td);ota.chmod(0o700)
            def server(p):
                self.wake_upload(p)
                request=ota/"auto-apply.request";request.write_text("b"*48+"\n");request.chmod(0o600)
                time.sleep(.15)
                self.assertFalse((ota/"auto-apply.idle").exists())
                p.sock.sendall(frame(1,{"type":"wake","state":"ignored"}))
                self.assertEqual(p.receive()[0],8)
                request.unlink()
            env=self.wake_env();env["S12_TEST_OTA_DIR"]=td
            r=self.invoke(server,["--wake-loop","--wake-rounds","1"],env_change=env)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn(b'"ota_idle_enter"',r.stdout)

    def test_wake_events_during_tts_are_rejected(self):
        def server(p):
            self.wake_upload(p)
            p.sock.sendall(frame(1, {"type": "tts", "state": "start"}) +
                           frame(2, self.packet) + frame(1, {"type": "wake", "state": "ignored"}))
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "1"], env_change=self.wake_env())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"wake event arrived during TTS", r.stderr)
        self.assertNotIn(b"tts_buffered", r.stdout)
        self.assertNotIn(b"turn_complete", r.stdout)

    def test_wake_server_timeout_is_one_bounded_failed_round(self):
        def server(p):
            self.wake_upload(p)
            time.sleep(2.2)
        r = self.invoke(server, ["--wake-loop", "--wake-rounds", "1"], env_change=self.wake_env())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"deadline expired", r.stderr)
        self.assertIn(b'"submitted_rounds":1', r.stdout)
        self.assertNotIn(b"turn_complete", r.stdout)

    def test_wake_cli_bounds_and_numeric_host_gate(self):
        for args in [["--wake-loop", "--seconds", "3"], ["--wake-loop", "--vad-rms", "29"],
                     ["--wake-loop", "--vad-rms", "3001"], ["--wake-loop", "--wake-rounds", "-1"],
                     ["--wake-loop", "--wake-rounds", "10001"], ["--listen-once", "--wake-rounds", "1"],
                     ["--wake-loop", "--text", "test"], ["--wake-loop", "--mic-channel", "6"]]:
            self.assertEqual(self.invoke(args=args).returncode, 2)
        r = self.invoke(args=["--wake-loop"], config_change={"url": "ws://localhost:9/xiaozhi/v1/"})
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"requires a numeric private IPv4", r.stderr)

    def test_listen_once_cli_bounds(self):
        for args in [["--listen-once", "--seconds", "2"], ["--listen-once", "--seconds", "11"],
                     ["--listen-once", "--mic-channel", "6"], ["--listen-once", "--mic-channel", "-1"],
                     ["--check-ws", "--seconds", "3"], ["--text", "test", "--listen-once"]]:
            self.assertEqual(self.invoke(args=args).returncode, 2)


if __name__ == "__main__":
    unittest.main()
