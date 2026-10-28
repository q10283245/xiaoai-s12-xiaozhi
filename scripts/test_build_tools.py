"""Offline-only build tests. Synthetic CPIO/configuration, no vendor files."""
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import unittest

import build_image as image
import prepare_config as config


def cpio_fixture(extra=()):
    entries = [('init', b'#!/bin/sh\nexit 1\n', stat.S_IFREG | 0o755),
               ('bin', b'', stat.S_IFDIR | 0o755),
               ('bin/busybox', b'synthetic busybox bytes', stat.S_IFREG | 0o755)]
    entries += list(extra)
    entries += [('TRAILER!!!', b'', 0)]
    blob = b''.join(image.newc(n, d, m, i + 1) for i, (n, d, m) in enumerate(entries))
    return blob + b'\0' * (image.boot.align(len(blob), 512) - len(blob))


def android_fixture():
    kernel, ramdisk, dtb = b'synthetic kernel', image.boot.gzip_deterministic(cpio_fixture()), b'synthetic DTB'
    h = bytearray(2048)
    h[:8] = b'ANDROID!'
    struct.pack_into('<10I', h, 8, len(kernel), 0x1000000, len(ramdisk), 0x2000000, len(dtb), 0, 0, 2048, 0, 0)
    h[576:596] = image.boot.android_id(kernel, ramdisk, dtb)
    return bytes(h) + b''.join(b + b'\0' * (image.boot.align(len(b), 2048) - len(b)) for b in (kernel, ramdisk, dtb))


class BuildToolTests(unittest.TestCase):
    def test_configuration_is_private_and_tokens_are_separate(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'private'
            result = config.prepare(p, 'ws://127.0.0.1:8000/xiaozhi/v1/', '02:00:00:00:00:01',
                                    'synthetic-wifi', 'synthetic-password', 'fake-upstream-token')
            self.assertFalse(result['device_access'])
            self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o700)
            self.assertTrue(all(stat.S_IMODE(f.stat().st_mode) == 0o600 for f in p.iterdir()))
            self.assertNotEqual((p / 'token').read_bytes(), (p / 'upstream-token').read_bytes())
            self.assertNotIn(b'synthetic-password', (p / 'wpa.conf').read_bytes())
            payload = image.config_payloads(p)
            self.assertEqual(payload['s12/private/token'][0], (p / 'token').read_bytes())

    def test_ssid_text_cannot_escape_wpa_network(self):
        ssid = 'synthetic"}\nnetwork={'
        wpa = config.make_wpa(ssid, 'synthetic-password')
        self.assertNotIn(ssid.encode(), wpa)
        self.assertEqual(wpa.count(b'network={'), 1)
        self.assertIn(ssid.encode().hex().encode(), wpa)

    def test_public_cleartext_and_dns_cleartext_rejected(self):
        for url in ('ws://8.8.8.8/xiaozhi/v1/', 'ws://localhost/xiaozhi/v1/', 'https://server.example/xiaozhi/v1/',
                    'wss://server.example:notaport/xiaozhi/v1/', 'wss://server.example:0/xiaozhi/v1/',
                    'wss://server.example:65536/xiaozhi/v1/', 'wss://server.example:/xiaozhi/v1/'):
            with self.assertRaises(ValueError):
                config.valid_server(url)
        self.assertEqual(config.valid_server('wss://server.example/xiaozhi/v1/?custom=value').query, 'custom=value')

    def test_wss_requires_public_ca_and_new_directory(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'config'
            with self.assertRaises(ValueError):
                config.prepare(output, 'wss://server.example/xiaozhi/v1/', '02:00:00:00:00:01', 'synthetic', 'synthetic-password')
            self.assertFalse(output.exists())
            output.mkdir()
            with self.assertRaises(FileExistsError):
                config.prepare(output, 'ws://127.0.0.1/xiaozhi/v1/', '02:00:00:00:00:01', 'synthetic', 'synthetic-password')

    def test_private_config_rejects_world_readable_files(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'config'
            config.prepare(p, 'ws://127.0.0.1/xiaozhi/v1/', '02:00:00:00:00:01', 'synthetic', 'synthetic-password')
            (p / 'token').chmod(0o644)
            with self.assertRaises(ValueError):
                image.config_payloads(p)

    def test_cpio_additions_preserve_original_members(self):
        original = cpio_fixture()
        cpio, names = image.extend(original, b'#!/bin/sh\nsynthetic\n', {'s12/test': (b'test', 0o600)})
        before = {e.name: e for e in image.boot.parse_cpio(original)[0]}
        after = {e.name: e for e in image.boot.parse_cpio(cpio)[0]}
        self.assertIn('s12/test', names)
        self.assertEqual(before[b'bin/busybox'].raw, after[b'bin/busybox'].raw)
        self.assertNotEqual(before[b'init'].data, after[b'init'].data)
        self.assertEqual(after[b's12/test'].mode, stat.S_IFREG | 0o600)

    def test_cpio_conflicts_and_unsafe_paths_rejected(self):
        for name in ('/absolute', '../escape', 'a/../escape'):
            with self.assertRaises(ValueError):
                image.newc(name, b'', 0o600, 1)
        conflicting = cpio_fixture((('s12', b'', stat.S_IFDIR | 0o700),))
        with self.assertRaises(ValueError):
            image.extend(conflicting, b'#!/bin/sh\n', {})

    def test_synthetic_android_repack_keeps_kernel_dtb_header(self):
        source = image.boot.parse_android(android_fixture())
        cpio, _ = image.extend(image.boot.gunzip_bounded(source.ramdisk), b'#!/bin/sh\nsynthetic\n', {})
        blob = image.boot.repack(source, image.boot.gzip_deterministic(cpio))
        result = image.boot.parse_android(blob)
        self.assertEqual(source.kernel, result.kernel)
        self.assertEqual(source.second, result.second)
        allowed = set(range(16, 20)) | set(range(576, 596))
        self.assertTrue(all(a == b for i, (a, b) in enumerate(zip(source.header, result.header)) if i not in allowed))
        with self.assertRaises(ValueError):
            image.boot.parse_android(blob + b'outside')

    def test_prompt_synthesis_is_bounded_reproducible(self):
        a = image.tone([(880, 100), (0, 30), (1320, 100)])
        self.assertEqual(a, image.tone([(880, 100), (0, 30), (1320, 100)]))
        samples = struct.unpack('<' + 'h' * (len(a) // 2), a)
        self.assertLessEqual(max(map(abs, samples)), 2000)
        self.assertEqual(len(a), 230 * 16 * 2)

    def test_runtime_shell_syntax_only(self):
        for path in (image.ROOT / 'runtime').glob('*.sh'):
            subprocess.run(['sh', '-n', str(path)], check=True, capture_output=True)
        init = (image.ROOT / 'runtime/init.sh').read_text()
        for forbidden in ('fw_env', 'nandwrite', 'flash_erase', 'ubiattach', 'reboot -f'):
            self.assertNotIn(forbidden, init)
        self.assertIn('mount -t squashfs -o ro', init)


if __name__ == '__main__':
    unittest.main()
